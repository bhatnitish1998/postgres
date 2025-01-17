/*-------------------------------------------------------------------------
 *
 * nbtree.c
 *	  Implementation of Lehman and Yao's btree management algorithm for
 *	  Postgres.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtree.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/relscan.h"
#include "access/xlog.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/condition_variable.h"
#include "storage/indexfsm.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"

///////////////////////////////////// ADDED CODE - START  ////////////////////////////////////////
#include "catalog/index.h"
#include "catalog/storage.h"
///////////////////////////////////// ADDED CODE - END  ////////////////////////////////////////

/*
 * BTPARALLEL_NOT_INITIALIZED indicates that the scan has not started.
 *
 * BTPARALLEL_ADVANCING indicates that some process is advancing the scan to
 * a new page; others must wait.
 *
 * BTPARALLEL_IDLE indicates that no backend is currently advancing the scan
 * to a new page; some process can start doing that.
 *
 * BTPARALLEL_DONE indicates that the scan is complete (including error exit).
 * We reach this state once for every distinct combination of array keys.
 */
typedef enum
{
	BTPARALLEL_NOT_INITIALIZED,
	BTPARALLEL_ADVANCING,
	BTPARALLEL_IDLE,
	BTPARALLEL_DONE
} BTPS_State;

/*
 * BTParallelScanDescData contains btree specific shared information required
 * for parallel scan.
 */
typedef struct BTParallelScanDescData
{
	BlockNumber btps_scanPage;	/* latest or next page to be scanned */
	BTPS_State	btps_pageStatus;	/* indicates whether next page is
									 * available for scan. see above for
									 * possible states of parallel scan. */
	int			btps_arrayKeyCount; /* count indicating number of array scan
									 * keys processed by parallel scan */
	slock_t		btps_mutex;		/* protects above variables */
	ConditionVariable btps_cv;	/* used to synchronize parallel scan */
}			BTParallelScanDescData;

typedef struct BTParallelScanDescData *BTParallelScanDesc;


static void btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
						 IndexBulkDeleteCallback callback, void *callback_state,
						 BTCycleId cycleid);
static void btvacuumpage(BTVacState *vstate, BlockNumber scanblkno);
static BTVacuumPosting btreevacuumposting(BTVacState *vstate,
										  IndexTuple posting,
										  OffsetNumber updatedoffset,
										  int *nremaining);

///////////////////////////////////// ADDED CODE - START  ////////////////////////////////////////

/*
 *  Check whether it is suitable to build lsm tree on the given relation
 *
 *  True : If LSM flag is set and relation name does not start with 'pg_'
 */
bool check_lsm(const char * relation_name)
{
    // avoid tables that start with pg_
    char check[3]={relation_name[0],relation_name[1],relation_name[2]};
    bool name_check = strcmp(check,"pg_") != 0;

    // check if lsm_tree_flag is set
    return lsm_tree_flag && name_check;
}

/*
 * Creates a lsm_meta_data structure at the end of passed BTMetaPage header and extends the lower
 * pointer of the header to include it
 */
lsm_meta_data* set_meta_in_metapage(Page page_t)
{

    // Get location just after existing metadata
    BTMetaPageData* meta_t =BTPageGetMeta(page_t);
    meta_t = meta_t+1;

    // Cast it to lsm_meta_data
    lsm_meta_data *lsm_md = (lsm_meta_data *)(meta_t);

    // change page header lower pointer
    PageHeader pageHeader = (PageHeader)page_t;
    pageHeader->pd_lower = pageHeader->pd_lower +sizeof(struct lsm_meta_data);

    return lsm_md;
}
/*
 * Returns the pointer to lsm_meta_data structure present in the passed BTMetaPage
 */
lsm_meta_data* get_meta_from_metapage(Page page_t)
{
    // Get location just after existing metadata
    BTMetaPageData* meta_t =BTPageGetMeta(page_t);
    meta_t = meta_t+1;
    // Cast it to lsm_meta_data
    lsm_meta_data *lsm_md = (lsm_meta_data *)(meta_t);
    return lsm_md;
}

/*
 * Initialize lsm metadata
 */
void initialize_meta(lsm_meta_data* x)
{
    x->l0_size=0;
    x->l1_size=0;
    x->l2_size=0;

    x->l0_max_size =L0_MAX_SIZE;
    x->l1_max_size =L1_MAX_SIZE;

    x->l0_id=InvalidOid;
    x->l1_id=InvalidOid;
    x->l2_id=InvalidOid;

    x->rel_id=InvalidOid;
}

/*
 * Function to print the sizes and Oids of LSM trees at various levels
 */
void print_meta_data(lsm_meta_data* lsm_md)
{
    printf("Oid: L0 = %d ; L1 = %d ; L2 = %d ;\n",lsm_md->l0_id,lsm_md->l1_id,lsm_md->l2_id);
    elog(NOTICE,"Oid: L0 = %d ; L1 = %d ; L2 = %d ;\n",lsm_md->l0_id,lsm_md->l1_id,lsm_md->l2_id);
    printf("Size: L0 = %d ; L1 = %d ; L2 = %d ;\n",lsm_md->l0_size,lsm_md->l1_size,lsm_md->l2_size);
    elog(NOTICE,"Size: L0 = %d ; L1 = %d ; L2 = %d ;\n",lsm_md->l0_size,lsm_md->l1_size,lsm_md->l2_size);
}

/*
 *  Creates a new index tree for Relation heapRel, at the level specified in the argument.
 *  Returns the Oid of the newly created tree
 */
Oid create_new_tree(Relation heapRel,Relation rel,int level)
{
    // get new name for index and copy the index
    char *oldname = rel->rd_rel->relname.data;
    char * newname = palloc(NAMEDATALEN);
    strcpy(newname,oldname);

    if(level ==1 )
        strcat(newname,"l1_");
    else if(level ==2)
        strcat(newname,"l2_");

    Oid new_tree =index_concurrently_create_copy(heapRel,rel->rd_id,rel->rd_rel->reltablespace,newname);

    // build index of current on l1;
    Relation t = index_open(new_tree,AccessExclusiveLock);
    index_build(heapRel,t, BuildIndexInfo(t),false,false);
    index_close(t,AccessExclusiveLock);

    pfree(newname);
    return new_tree;
}

/*
 *  Merges the index relation smaller into the larger relation
 *  Parameters:
 *      heapRel : Relation on which the index is built.
 *      smaller : Smaller Relation which has to be merged
 *      larger : Oid of the larger Relation to merge into
 */
void merge_tree(Relation heapRel, Relation smaller, Oid larger)
{
    // open lower level index with Oid.
    Relation lrg = index_open(larger,AccessExclusiveLock);

    // set want index tuple to true and restart
    IndexScanDesc scan = index_beginscan(heapRel,smaller,SnapshotAny,0,0);
    scan->xs_want_itup=true;
    btrescan(scan,NULL,0,0,0);

    // point to first tuple
    _bt_first(scan, ForwardScanDirection);
    // repeatedly scan all indexes of smaller tree and add to larger tree
    do
    {
        IndexTuple itup_local = scan->xs_itup;
        _bt_doinsert(lrg,itup_local,UNIQUE_CHECK_NO,false,heapRel);
    }
    while(_bt_next(scan,ForwardScanDirection));
    index_endscan(scan);
    index_close(lrg,AccessExclusiveLock);

}

/*
 *  Clears the given index
 *  Parameters:
 *      heapRel : Relation on which the index is built.
 *      rel : L0 index (since metadata is stored on top of L0)
 *      to_clear : Relation which is to be cleared
 *      buffer_t : to release the lock and reacquire after clearing
 *      lsm_md_p : Pointer to the lsm_md_p meta data.
 */

Buffer clear_index(Relation heapRel,Relation rel,Relation to_clear, Buffer buffer_t,lsm_meta_data** lsm_md_p)
{
    printf("-CLEAR BEGIN -\n");
    elog(NOTICE,"-CLEAR BEGIN -\n");
    // copy metadata
    lsm_meta_data *lsm_copy = palloc(sizeof(struct lsm_meta_data));
    memcpy(lsm_copy,*lsm_md_p,sizeof (struct lsm_meta_data));

    //release the buffer
    _bt_relbuf(rel,buffer_t);

    printf("Truncating index:%d to 0 blocks\n",to_clear->rd_id);
    elog(NOTICE,"Truncating index:%d to 0 blocks\n",to_clear->rd_id);
    // truncate the relation
    RelationTruncate(to_clear,0);
    IndexInfo* index_info = BuildDummyIndexInfo(to_clear);
    index_build(heapRel,to_clear,index_info,true,false);


    // reacquire the buffer and copy back metadata
    buffer_t= _bt_getbuf(rel,BTREE_METAPAGE,BT_WRITE);
    Page page_t =BufferGetPage(buffer_t);
    *lsm_md_p = get_meta_from_metapage(page_t);
    memcpy(*lsm_md_p,lsm_copy,sizeof(struct lsm_meta_data));

    pfree(lsm_copy);

    printf("-CLEAR END -\n");
    elog(NOTICE,"-CLEAR END -\n");

    return buffer_t;
}

///////////////////////////////////// ADDED CODE - END  ////////////////////////////////////////

/*
 * Btree handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
bthandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amoptsprocnum = BTOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = true;
	amroutine->amcanunique = true;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = true;
	amroutine->amsearchnulls = true;
	amroutine->amstorage = false;
	amroutine->amclusterable = true;
	amroutine->ampredlocks = true;
	amroutine->amcanparallel = true;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions =
		VACUUM_OPTION_PARALLEL_BULKDEL | VACUUM_OPTION_PARALLEL_COND_CLEANUP;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = btbuild;
	amroutine->ambuildempty = btbuildempty;
	amroutine->aminsert = btinsert;
	amroutine->ambulkdelete = btbulkdelete;
	amroutine->amvacuumcleanup = btvacuumcleanup;
	amroutine->amcanreturn = btcanreturn;
	amroutine->amcostestimate = btcostestimate;
	amroutine->amoptions = btoptions;
	amroutine->amproperty = btproperty;
	amroutine->ambuildphasename = btbuildphasename;
	amroutine->amvalidate = btvalidate;
	amroutine->amadjustmembers = btadjustmembers;
	amroutine->ambeginscan = btbeginscan;
	amroutine->amrescan = btrescan;
	amroutine->amgettuple = btgettuple;
	amroutine->amgetbitmap = btgetbitmap;
	amroutine->amendscan = btendscan;
	amroutine->ammarkpos = btmarkpos;
	amroutine->amrestrpos = btrestrpos;
	amroutine->amestimateparallelscan = btestimateparallelscan;
	amroutine->aminitparallelscan = btinitparallelscan;
	amroutine->amparallelrescan = btparallelrescan;

	PG_RETURN_POINTER(amroutine);
}

/*
 *	btbuildempty() -- build an empty btree index in the initialization fork
 */
void
btbuildempty(Relation index)
{
	Page		metapage;

	/* Construct metapage. */
	metapage = (Page) palloc(BLCKSZ);
	_bt_initmetapage(metapage, P_NONE, 0, _bt_allequalimage(index, false));

    ///////////////////////////////////// ADDED CODE - START  ////////////////////////////////////////

    if(lsm_tree_flag)
    {
        printf("----------------------BUILD EMPTY BEGIN--------------------\n");

        // initialize metadata
        lsm_meta_data *lsm_md = set_meta_in_metapage(metapage);
        printf("LSM meta data location: %x to %x\n",lsm_md,lsm_md+sizeof(struct lsm_meta_data));
        initialize_meta(lsm_md);

        // set lsm meta data
        lsm_md->l0_id = index->rd_id;
        lsm_md->rel_id = index->rd_index->indrelid;

        print_meta_data(lsm_md);
        printf("----------------------BUILD EMPTY END--------------------\n");
    }
///////////////////////////////////// ADDED CODE - END  ////////////////////////////////////////

	/*
	 * Write the page and log it.  It might seem that an immediate sync would
	 * be sufficient to guarantee that the file exists on disk, but recovery
	 * itself might remove it while replaying, for example, an
	 * XLOG_DBASE_CREATE or XLOG_TBLSPC_CREATE record.  Therefore, we need
	 * this even when wal_level=minimal.
	 */
	PageSetChecksumInplace(metapage, BTREE_METAPAGE);
	smgrwrite(RelationGetSmgr(index), INIT_FORKNUM, BTREE_METAPAGE,
			  (char *) metapage, true);
	log_newpage(&RelationGetSmgr(index)->smgr_rnode.node, INIT_FORKNUM,
				BTREE_METAPAGE, metapage, true);

	/*
	 * An immediate sync is required even if we xlog'd the page, because the
	 * write did not go through shared_buffers and therefore a concurrent
	 * checkpoint may have moved the redo pointer past our xlog record.
	 */
	smgrimmedsync(RelationGetSmgr(index), INIT_FORKNUM);
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, and put it there.
 */
bool
btinsert(Relation rel, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 bool indexUnchanged,
		 IndexInfo *indexInfo)
{
	bool		result;
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_form_tuple(RelationGetDescr(rel), values, isnull);
	itup->t_tid = *ht_ctid;

	result = _bt_doinsert(rel, itup, checkUnique, indexUnchanged, heapRel);

	pfree(itup);

    ///////////////////////////////////// ADDED CODE - START  ////////////////////////////////////////

    if(check_lsm(heapRel->rd_rel->relname.data))
    {
        printf("----------------------INSERT BEGIN--------------------\n");
        elog(NOTICE,"----------------------INSERT BEGIN--------------------\n");

        // Read lsm meta data
        Buffer buffer_t = _bt_getbuf(rel,BTREE_METAPAGE,BT_WRITE);
        Page page_t = BufferGetPage(buffer_t);
        struct lsm_meta_data *lsm_md = get_meta_from_metapage(page_t);

        // increment level 0 tree size by 1
        lsm_md->l0_size= lsm_md->l0_size+ 1;
        print_meta_data(lsm_md);

        // check overflow
        if(lsm_md->l0_size >= lsm_md->l0_max_size) {
            printf("LSM 0 overflow detected\n");
            elog(NOTICE,"LSM 0 overflow detected\n");

            // check if l1 tree does not exist
            if (lsm_md->l1_id == InvalidOid) {
                printf("Creating new LSM 1 tree\n");
                elog(NOTICE,"Creating new LSM 1 tree\n");
                // create l1 tree
                lsm_md->l1_id = create_new_tree(heapRel, rel, 1);
            }

            // merge l0 to l1 tree
            printf("Merging L0:%d to  l1:%d\n", lsm_md->l0_id, lsm_md->l1_id);
            elog(NOTICE,"Merging L0:%d  to  l1:%d\n", lsm_md->l0_id, lsm_md->l1_id);
            merge_tree(heapRel, rel, lsm_md->l1_id);
            lsm_md->l1_size += lsm_md->l0_size;

            // clear L0 index

            printf("Clearing l0 index\n");
            elog(NOTICE,"Clearing l0 index\n");
            buffer_t = clear_index(heapRel, rel,rel, buffer_t, &lsm_md);
            lsm_md->l0_size = 0;
            printf("Clear successful\n");
            elog(NOTICE,"Clear successful\n");
            print_meta_data(lsm_md);
        }

            // check if overflow from L1
        if(lsm_md->l1_size>= lsm_md->l1_max_size)
        {  printf("LSM 1 overflow detected\n");
            elog(NOTICE,"LSM 1 overflow detected\n");
            // check if l2 tree does not exist
            if(lsm_md->l2_id== InvalidOid)
            {
                printf("Creating new LSM 2 tree\n");
                elog(NOTICE,"Creating new LSM 2 tree\n");
                // create l2 tree
                lsm_md->l2_id = create_new_tree(heapRel,rel,2);
            }

            // merge l1 to l2 tree
            printf("Merging L1 oid : %d ,  l2 oid : %d\n",lsm_md->l1_id,lsm_md->l2_id);
            elog(NOTICE,"Merging L1 oid : %d ,  l2 oid : %d\n",lsm_md->l1_id,lsm_md->l2_id);
            Relation sml = index_open(lsm_md->l1_id,AccessExclusiveLock);
            merge_tree(heapRel,sml,lsm_md->l2_id);
            index_close(sml,AccessExclusiveLock);

            //Update size
            lsm_md->l2_size+= lsm_md->l1_size;
            printf("finished adding L1:%d entries to L2:%d\n",lsm_md->l1_id,lsm_md->l2_id);
            elog(NOTICE,"finished adding L1:%d entries to L2:%d\n",lsm_md->l1_id,lsm_md->l2_id);

            // clear L1 index
            printf("Clearing l1 index\n");
            elog(NOTICE,"Clearing l1 index\n");
            Relation l1_rel = index_open(lsm_md->l1_id,AccessExclusiveLock);
            buffer_t = clear_index(heapRel,rel, l1_rel, buffer_t,&lsm_md);
            lsm_md->l1_size =0;
            index_close(l1_rel,AccessExclusiveLock);
            printf("Clear successful\n");
            elog(NOTICE,"Clear successful\n");
            print_meta_data(lsm_md);
        }

        MarkBufferDirty(buffer_t);
        _bt_relbuf(rel,buffer_t);
        printf("----------------------INSERT END--------------------\n");
        elog(NOTICE,"----------------------INSERT END--------------------\n");
    }
///////////////////////////////////// ADDED CODE - END  ////////////////////////////////////////

	return result;
}

/*
 *	btgettuple() -- Get the next tuple in the scan.
 */
bool
btgettuple(IndexScanDesc scan, ScanDirection dir)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	bool		res;

	/* btree indexes are never lossy */
	scan->xs_recheck = false;

	/*
	 * If we have any array keys, initialize them during first call for a
	 * scan.  We can't do this in btrescan because we don't know the scan
	 * direction at that time.
	 */
	if (so->numArrayKeys && !BTScanPosIsValid(so->currPos))
	{
		/* punt if we have any unsatisfiable array keys */
		if (so->numArrayKeys < 0)
			return false;

		_bt_start_array_keys(scan, dir);
	}

	/* This loop handles advancing to the next array elements, if any */
	do
	{
		/*
		 * If we've already initialized this scan, we can just advance it in
		 * the appropriate direction.  If we haven't done so yet, we call
		 * _bt_first() to get the first item in the scan.
		 */
		if (!BTScanPosIsValid(so->currPos))
			res = _bt_first(scan, dir);
		else
		{
			/*
			 * Check to see if we should kill the previously-fetched tuple.
			 */
			if (scan->kill_prior_tuple)
			{
				/*
				 * Yes, remember it for later. (We'll deal with all such
				 * tuples at once right before leaving the index page.)  The
				 * test for numKilled overrun is not just paranoia: if the
				 * caller reverses direction in the indexscan then the same
				 * item might get entered multiple times. It's not worth
				 * trying to optimize that, so we don't detect it, but instead
				 * just forget any excess entries.
				 */
				if (so->killedItems == NULL)
					so->killedItems = (int *)
						palloc(MaxTIDsPerBTreePage * sizeof(int));
				if (so->numKilled < MaxTIDsPerBTreePage)
					so->killedItems[so->numKilled++] = so->currPos.itemIndex;
			}

			/*
			 * Now continue the scan.
			 */
			res = _bt_next(scan, dir);
		}

		/* If we have a tuple, return it ... */
		if (res)
			break;
		/* ... otherwise see if we have more array keys to deal with */
	} while (so->numArrayKeys && _bt_advance_array_keys(scan, dir));

	return res;
}

/*
 * btgetbitmap() -- gets all matching tuples, and adds them to a bitmap
 */
int64
btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	int64		ntids = 0;
	ItemPointer heapTid;

	/*
	 * If we have any array keys, initialize them.
	 */
	if (so->numArrayKeys)
	{
		/* punt if we have any unsatisfiable array keys */
		if (so->numArrayKeys < 0)
			return ntids;

		_bt_start_array_keys(scan, ForwardScanDirection);
	}

	/* This loop handles advancing to the next array elements, if any */
	do
	{
		/* Fetch the first page & tuple */
		if (_bt_first(scan, ForwardScanDirection))
		{
			/* Save tuple ID, and continue scanning */
			heapTid = &scan->xs_heaptid;
			tbm_add_tuples(tbm, heapTid, 1, false);
			ntids++;

			for (;;)
			{
				/*
				 * Advance to next tuple within page.  This is the same as the
				 * easy case in _bt_next().
				 */
				if (++so->currPos.itemIndex > so->currPos.lastItem)
				{
					/* let _bt_next do the heavy lifting */
					if (!_bt_next(scan, ForwardScanDirection))
						break;
				}

				/* Save tuple ID, and continue scanning */
				heapTid = &so->currPos.items[so->currPos.itemIndex].heapTid;
				tbm_add_tuples(tbm, heapTid, 1, false);
				ntids++;
			}
		}
		/* Now see if we have more array keys to deal with */
	} while (so->numArrayKeys && _bt_advance_array_keys(scan, ForwardScanDirection));

	return ntids;
}

/*
 *	btbeginscan() -- start a scan on a btree index
 */
IndexScanDesc
btbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	BTScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
	BTScanPosInvalidate(so->currPos);
	BTScanPosInvalidate(so->markPos);
	if (scan->numberOfKeys > 0)
		so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
	else
		so->keyData = NULL;

	so->arrayKeyData = NULL;	/* assume no array keys for now */
	so->arraysStarted = false;
	so->numArrayKeys = 0;
	so->arrayKeys = NULL;
	so->arrayContext = NULL;

	so->killedItems = NULL;		/* until needed */
	so->numKilled = 0;

	/*
	 * We don't know yet whether the scan will be index-only, so we do not
	 * allocate the tuple workspace arrays until btrescan.  However, we set up
	 * scan->xs_itupdesc whether we'll need it or not, since that's so cheap.
	 */
	so->currTuples = so->markTuples = NULL;

	scan->xs_itupdesc = RelationGetDescr(rel);

	scan->opaque = so;

	return scan;
}

/*
 *	btrescan() -- rescan an index relation
 */
void
btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (BTScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_bt_killitems(scan);
		BTScanPosUnpinIfPinned(so->currPos);
		BTScanPosInvalidate(so->currPos);
	}

	so->markItemIndex = -1;
	so->arrayKeyCount = 0;
	BTScanPosUnpinIfPinned(so->markPos);
	BTScanPosInvalidate(so->markPos);

	/*
	 * Allocate tuple workspace arrays, if needed for an index-only scan and
	 * not already done in a previous rescan call.  To save on palloc
	 * overhead, both workspaces are allocated as one palloc block; only this
	 * function and btendscan know that.
	 *
	 * NOTE: this data structure also makes it safe to return data from a
	 * "name" column, even though btree name_ops uses an underlying storage
	 * datatype of cstring.  The risk there is that "name" is supposed to be
	 * padded to NAMEDATALEN, but the actual index tuple is probably shorter.
	 * However, since we only return data out of tuples sitting in the
	 * currTuples array, a fetch of NAMEDATALEN bytes can at worst pull some
	 * data out of the markTuples array --- running off the end of memory for
	 * a SIGSEGV is not possible.  Yeah, this is ugly as sin, but it beats
	 * adding special-case treatment for name_ops elsewhere.
	 */
	if (scan->xs_want_itup && so->currTuples == NULL)
	{
		so->currTuples = (char *) palloc(BLCKSZ * 2);
		so->markTuples = so->currTuples + BLCKSZ;
	}

	/*
	 * Reset the scan keys
	 */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	so->numberOfKeys = 0;		/* until _bt_preprocess_keys sets it */

	/* If any keys are SK_SEARCHARRAY type, set up array-key info */
	_bt_preprocess_array_keys(scan);
}

/*
 *	btendscan() -- close down a scan
 */
void
btendscan(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (BTScanPosIsValid(so->currPos))
	{
		/* Before leaving current page, deal with any killed items */
		if (so->numKilled > 0)
			_bt_killitems(scan);
		BTScanPosUnpinIfPinned(so->currPos);
	}

	so->markItemIndex = -1;
	BTScanPosUnpinIfPinned(so->markPos);

	/* No need to invalidate positions, the RAM is about to be freed. */

	/* Release storage */
	if (so->keyData != NULL)
		pfree(so->keyData);
	/* so->arrayKeyData and so->arrayKeys are in arrayContext */
	if (so->arrayContext != NULL)
		MemoryContextDelete(so->arrayContext);
	if (so->killedItems != NULL)
		pfree(so->killedItems);
	if (so->currTuples != NULL)
		pfree(so->currTuples);
	/* so->markTuples should not be pfree'd, see btrescan */
	pfree(so);
}

/*
 *	btmarkpos() -- save current scan position
 */
void
btmarkpos(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* There may be an old mark with a pin (but no lock). */
	BTScanPosUnpinIfPinned(so->markPos);

	/*
	 * Just record the current itemIndex.  If we later step to next page
	 * before releasing the marked position, _bt_steppage makes a full copy of
	 * the currPos struct in markPos.  If (as often happens) the mark is moved
	 * before we leave the page, we don't have to do that work.
	 */
	if (BTScanPosIsValid(so->currPos))
		so->markItemIndex = so->currPos.itemIndex;
	else
	{
		BTScanPosInvalidate(so->markPos);
		so->markItemIndex = -1;
	}

	/* Also record the current positions of any array keys */
	if (so->numArrayKeys)
		_bt_mark_array_keys(scan);
}

/*
 *	btrestrpos() -- restore scan to last saved position
 */
void
btrestrpos(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;

	/* Restore the marked positions of any array keys */
	if (so->numArrayKeys)
		_bt_restore_array_keys(scan);

	if (so->markItemIndex >= 0)
	{
		/*
		 * The scan has never moved to a new page since the last mark.  Just
		 * restore the itemIndex.
		 *
		 * NB: In this case we can't count on anything in so->markPos to be
		 * accurate.
		 */
		so->currPos.itemIndex = so->markItemIndex;
	}
	else
	{
		/*
		 * The scan moved to a new page after last mark or restore, and we are
		 * now restoring to the marked page.  We aren't holding any read
		 * locks, but if we're still holding the pin for the current position,
		 * we must drop it.
		 */
		if (BTScanPosIsValid(so->currPos))
		{
			/* Before leaving current page, deal with any killed items */
			if (so->numKilled > 0)
				_bt_killitems(scan);
			BTScanPosUnpinIfPinned(so->currPos);
		}

		if (BTScanPosIsValid(so->markPos))
		{
			/* bump pin on mark buffer for assignment to current buffer */
			if (BTScanPosIsPinned(so->markPos))
				IncrBufferRefCount(so->markPos.buf);
			memcpy(&so->currPos, &so->markPos,
				   offsetof(BTScanPosData, items[1]) +
				   so->markPos.lastItem * sizeof(BTScanPosItem));
			if (so->currTuples)
				memcpy(so->currTuples, so->markTuples,
					   so->markPos.nextTupleOffset);
		}
		else
			BTScanPosInvalidate(so->currPos);
	}
}

/*
 * btestimateparallelscan -- estimate storage for BTParallelScanDescData
 */
Size
btestimateparallelscan(void)
{
	return sizeof(BTParallelScanDescData);
}

/*
 * btinitparallelscan -- initialize BTParallelScanDesc for parallel btree scan
 */
void
btinitparallelscan(void *target)
{
	BTParallelScanDesc bt_target = (BTParallelScanDesc) target;

	SpinLockInit(&bt_target->btps_mutex);
	bt_target->btps_scanPage = InvalidBlockNumber;
	bt_target->btps_pageStatus = BTPARALLEL_NOT_INITIALIZED;
	bt_target->btps_arrayKeyCount = 0;
	ConditionVariableInit(&bt_target->btps_cv);
}

/*
 *	btparallelrescan() -- reset parallel scan
 */
void
btparallelrescan(IndexScanDesc scan)
{
	BTParallelScanDesc btscan;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;

	Assert(parallel_scan);

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * In theory, we don't need to acquire the spinlock here, because there
	 * shouldn't be any other workers running at this point, but we do so for
	 * consistency.
	 */
	SpinLockAcquire(&btscan->btps_mutex);
	btscan->btps_scanPage = InvalidBlockNumber;
	btscan->btps_pageStatus = BTPARALLEL_NOT_INITIALIZED;
	btscan->btps_arrayKeyCount = 0;
	SpinLockRelease(&btscan->btps_mutex);
}

/*
 * _bt_parallel_seize() -- Begin the process of advancing the scan to a new
 *		page.  Other scans must wait until we call _bt_parallel_release()
 *		or _bt_parallel_done().
 *
 * The return value is true if we successfully seized the scan and false
 * if we did not.  The latter case occurs if no pages remain for the current
 * set of scankeys.
 *
 * If the return value is true, *pageno returns the next or current page
 * of the scan (depending on the scan direction).  An invalid block number
 * means the scan hasn't yet started, and P_NONE means we've reached the end.
 * The first time a participating process reaches the last page, it will return
 * true and set *pageno to P_NONE; after that, further attempts to seize the
 * scan will return false.
 *
 * Callers should ignore the value of pageno if the return value is false.
 */
bool
_bt_parallel_seize(IndexScanDesc scan, BlockNumber *pageno)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	BTPS_State	pageStatus;
	bool		exit_loop = false;
	bool		status = true;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;

	*pageno = P_NONE;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	while (1)
	{
		SpinLockAcquire(&btscan->btps_mutex);
		pageStatus = btscan->btps_pageStatus;

		if (so->arrayKeyCount < btscan->btps_arrayKeyCount)
		{
			/* Parallel scan has already advanced to a new set of scankeys. */
			status = false;
		}
		else if (pageStatus == BTPARALLEL_DONE)
		{
			/*
			 * We're done with this set of scankeys.  This may be the end, or
			 * there could be more sets to try.
			 */
			status = false;
		}
		else if (pageStatus != BTPARALLEL_ADVANCING)
		{
			/*
			 * We have successfully seized control of the scan for the purpose
			 * of advancing it to a new page!
			 */
			btscan->btps_pageStatus = BTPARALLEL_ADVANCING;
			*pageno = btscan->btps_scanPage;
			exit_loop = true;
		}
		SpinLockRelease(&btscan->btps_mutex);
		if (exit_loop || !status)
			break;
		ConditionVariableSleep(&btscan->btps_cv, WAIT_EVENT_BTREE_PAGE);
	}
	ConditionVariableCancelSleep();

	return status;
}

/*
 * _bt_parallel_release() -- Complete the process of advancing the scan to a
 *		new page.  We now have the new value btps_scanPage; some other backend
 *		can now begin advancing the scan.
 */
void
_bt_parallel_release(IndexScanDesc scan, BlockNumber scan_page)
{
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	SpinLockAcquire(&btscan->btps_mutex);
	btscan->btps_scanPage = scan_page;
	btscan->btps_pageStatus = BTPARALLEL_IDLE;
	SpinLockRelease(&btscan->btps_mutex);
	ConditionVariableSignal(&btscan->btps_cv);
}

/*
 * _bt_parallel_done() -- Mark the parallel scan as complete.
 *
 * When there are no pages left to scan, this function should be called to
 * notify other workers.  Otherwise, they might wait forever for the scan to
 * advance to the next page.
 */
void
_bt_parallel_done(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;
	bool		status_changed = false;

	/* Do nothing, for non-parallel scans */
	if (parallel_scan == NULL)
		return;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	/*
	 * Mark the parallel scan as done for this combination of scan keys,
	 * unless some other process already did so.  See also
	 * _bt_advance_array_keys.
	 */
	SpinLockAcquire(&btscan->btps_mutex);
	if (so->arrayKeyCount >= btscan->btps_arrayKeyCount &&
		btscan->btps_pageStatus != BTPARALLEL_DONE)
	{
		btscan->btps_pageStatus = BTPARALLEL_DONE;
		status_changed = true;
	}
	SpinLockRelease(&btscan->btps_mutex);

	/* wake up all the workers associated with this parallel scan */
	if (status_changed)
		ConditionVariableBroadcast(&btscan->btps_cv);
}

/*
 * _bt_parallel_advance_array_keys() -- Advances the parallel scan for array
 *			keys.
 *
 * Updates the count of array keys processed for both local and parallel
 * scans.
 */
void
_bt_parallel_advance_array_keys(IndexScanDesc scan)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	ParallelIndexScanDesc parallel_scan = scan->parallel_scan;
	BTParallelScanDesc btscan;

	btscan = (BTParallelScanDesc) OffsetToPointer((void *) parallel_scan,
												  parallel_scan->ps_offset);

	so->arrayKeyCount++;
	SpinLockAcquire(&btscan->btps_mutex);
	if (btscan->btps_pageStatus == BTPARALLEL_DONE)
	{
		btscan->btps_scanPage = InvalidBlockNumber;
		btscan->btps_pageStatus = BTPARALLEL_NOT_INITIALIZED;
		btscan->btps_arrayKeyCount++;
	}
	SpinLockRelease(&btscan->btps_mutex);
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
btbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	rel = info->index;
	BTCycleId	cycleid;

	/* allocate stats if first time through, else re-use existing struct */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Establish the vacuum cycle ID to use for this scan */
	/* The ENSURE stuff ensures we clean up shared memory on failure */
	PG_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	{
		cycleid = _bt_start_vacuum(rel);

		btvacuumscan(info, stats, callback, callback_state, cycleid);
	}
	PG_END_ENSURE_ERROR_CLEANUP(_bt_end_vacuum_callback, PointerGetDatum(rel));
	_bt_end_vacuum(rel);

	return stats;
}

/*
 * Post-VACUUM cleanup.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
IndexBulkDeleteResult *
btvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	BlockNumber num_delpages;

	/* No-op in ANALYZE ONLY mode */
	if (info->analyze_only)
		return stats;

	/*
	 * If btbulkdelete was called, we need not do anything (we just maintain
	 * the information used within _bt_vacuum_needs_cleanup() by calling
	 * _bt_set_cleanup_info() below).
	 *
	 * If btbulkdelete was _not_ called, then we have a choice to make: we
	 * must decide whether or not a btvacuumscan() call is needed now (i.e.
	 * whether the ongoing VACUUM operation can entirely avoid a physical scan
	 * of the index).  A call to _bt_vacuum_needs_cleanup() decides it for us
	 * now.
	 */
	if (stats == NULL)
	{
		/* Check if VACUUM operation can entirely avoid btvacuumscan() call */
		if (!_bt_vacuum_needs_cleanup(info->index))
			return NULL;

		/*
		 * Since we aren't going to actually delete any leaf items, there's no
		 * need to go through all the vacuum-cycle-ID pushups here.
		 *
		 * Posting list tuples are a source of inaccuracy for cleanup-only
		 * scans.  btvacuumscan() will assume that the number of index tuples
		 * from each page can be used as num_index_tuples, even though
		 * num_index_tuples is supposed to represent the number of TIDs in the
		 * index.  This naive approach can underestimate the number of tuples
		 * in the index significantly.
		 *
		 * We handle the problem by making num_index_tuples an estimate in
		 * cleanup-only case.
		 */
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
		btvacuumscan(info, stats, NULL, NULL, 0);
		stats->estimated_count = true;
	}

	/*
	 * Maintain num_delpages value in metapage for _bt_vacuum_needs_cleanup().
	 *
	 * num_delpages is the number of deleted pages now in the index that were
	 * not safe to place in the FSM to be recycled just yet.  num_delpages is
	 * greater than 0 only when _bt_pagedel() actually deleted pages during
	 * our call to btvacuumscan().  Even then, _bt_pendingfsm_finalize() must
	 * have failed to place any newly deleted pages in the FSM just moments
	 * ago.  (Actually, there are edge cases where recycling of the current
	 * VACUUM's newly deleted pages does not even become safe by the time the
	 * next VACUUM comes around.  See nbtree/README.)
	 */
	Assert(stats->pages_deleted >= stats->pages_free);
	num_delpages = stats->pages_deleted - stats->pages_free;
	_bt_set_cleanup_info(info->index, num_delpages);

	/*
	 * It's quite possible for us to be fooled by concurrent page splits into
	 * double-counting some index tuples, so disbelieve any total that exceeds
	 * the underlying heap's count ... if we know that accurately.  Otherwise
	 * this might just make matters worse.
	 */
	if (!info->estimated_count)
	{
		if (stats->num_index_tuples > info->num_heap_tuples)
			stats->num_index_tuples = info->num_heap_tuples;
	}

	return stats;
}

/*
 * btvacuumscan --- scan the index for VACUUMing purposes
 *
 * This combines the functions of looking for leaf tuples that are deletable
 * according to the vacuum callback, looking for empty pages that can be
 * deleted, and looking for old deleted pages that can be recycled.  Both
 * btbulkdelete and btvacuumcleanup invoke this (the latter only if no
 * btbulkdelete call occurred and _bt_vacuum_needs_cleanup returned true).
 *
 * The caller is responsible for initially allocating/zeroing a stats struct
 * and for obtaining a vacuum cycle ID if necessary.
 */
static void
btvacuumscan(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state,
			 BTCycleId cycleid)
{
	Relation	rel = info->index;
	BTVacState	vstate;
	BlockNumber num_pages;
	BlockNumber scanblkno;
	bool		needLock;

	/*
	 * Reset fields that track information about the entire index now.  This
	 * avoids double-counting in the case where a single VACUUM command
	 * requires multiple scans of the index.
	 *
	 * Avoid resetting the tuples_removed and pages_newly_deleted fields here,
	 * since they track information about the VACUUM command, and so must last
	 * across each call to btvacuumscan().
	 *
	 * (Note that pages_free is treated as state about the whole index, not
	 * the current VACUUM.  This is appropriate because RecordFreeIndexPage()
	 * calls are idempotent, and get repeated for the same deleted pages in
	 * some scenarios.  The point for us is to track the number of recyclable
	 * pages in the index at the end of the VACUUM command.)
	 */
	stats->num_pages = 0;
	stats->num_index_tuples = 0;
	stats->pages_deleted = 0;
	stats->pages_free = 0;

	/* Set up info to pass down to btvacuumpage */
	vstate.info = info;
	vstate.stats = stats;
	vstate.callback = callback;
	vstate.callback_state = callback_state;
	vstate.cycleid = cycleid;

	/* Create a temporary memory context to run _bt_pagedel in */
	vstate.pagedelcontext = AllocSetContextCreate(CurrentMemoryContext,
												  "_bt_pagedel",
												  ALLOCSET_DEFAULT_SIZES);

	/* Initialize vstate fields used by _bt_pendingfsm_finalize */
	vstate.bufsize = 0;
	vstate.maxbufsize = 0;
	vstate.pendingpages = NULL;
	vstate.npendingpages = 0;
	/* Consider applying _bt_pendingfsm_finalize optimization */
	_bt_pendingfsm_init(rel, &vstate, (callback == NULL));

	/*
	 * The outer loop iterates over all index pages except the metapage, in
	 * physical order (we hope the kernel will cooperate in providing
	 * read-ahead for speed).  It is critical that we visit all leaf pages,
	 * including ones added after we start the scan, else we might fail to
	 * delete some deletable tuples.  Hence, we must repeatedly check the
	 * relation length.  We must acquire the relation-extension lock while
	 * doing so to avoid a race condition: if someone else is extending the
	 * relation, there is a window where bufmgr/smgr have created a new
	 * all-zero page but it hasn't yet been write-locked by _bt_getbuf(). If
	 * we manage to scan such a page here, we'll improperly assume it can be
	 * recycled.  Taking the lock synchronizes things enough to prevent a
	 * problem: either num_pages won't include the new page, or _bt_getbuf
	 * already has write lock on the buffer and it will be fully initialized
	 * before we can examine it.  (See also vacuumlazy.c, which has the same
	 * issue.)	Also, we need not worry if a page is added immediately after
	 * we look; the page splitting code already has write-lock on the left
	 * page before it adds a right page, so we must already have processed any
	 * tuples due to be moved into such a page.
	 *
	 * We can skip locking for new or temp relations, however, since no one
	 * else could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(rel);

	scanblkno = BTREE_METAPAGE + 1;
	for (;;)
	{
		/* Get the current relation length */
		if (needLock)
			LockRelationForExtension(rel, ExclusiveLock);
		num_pages = RelationGetNumberOfBlocks(rel);
		if (needLock)
			UnlockRelationForExtension(rel, ExclusiveLock);

		if (info->report_progress)
			pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_TOTAL,
										 num_pages);

		/* Quit if we've scanned the whole relation */
		if (scanblkno >= num_pages)
			break;
		/* Iterate over pages, then loop back to recheck length */
		for (; scanblkno < num_pages; scanblkno++)
		{
			btvacuumpage(&vstate, scanblkno);
			if (info->report_progress)
				pgstat_progress_update_param(PROGRESS_SCAN_BLOCKS_DONE,
											 scanblkno);
		}
	}

	/* Set statistics num_pages field to final size of index */
	stats->num_pages = num_pages;

	MemoryContextDelete(vstate.pagedelcontext);

	/*
	 * If there were any calls to _bt_pagedel() during scan of the index then
	 * see if any of the resulting pages can be placed in the FSM now.  When
	 * it's not safe we'll have to leave it up to a future VACUUM operation.
	 *
	 * Finally, if we placed any pages in the FSM (either just now or during
	 * the scan), forcibly update the upper-level FSM pages to ensure that
	 * searchers can find them.
	 */
	_bt_pendingfsm_finalize(rel, &vstate);
	if (stats->pages_free > 0)
		IndexFreeSpaceMapVacuum(rel);
}

/*
 * btvacuumpage --- VACUUM one page
 *
 * This processes a single page for btvacuumscan().  In some cases we must
 * backtrack to re-examine and VACUUM pages that were the scanblkno during
 * a previous call here.  This is how we handle page splits (that happened
 * after our cycleid was acquired) whose right half page happened to reuse
 * a block that we might have processed at some point before it was
 * recycled (i.e. before the page split).
 */
static void
btvacuumpage(BTVacState *vstate, BlockNumber scanblkno)
{
	IndexVacuumInfo *info = vstate->info;
	IndexBulkDeleteResult *stats = vstate->stats;
	IndexBulkDeleteCallback callback = vstate->callback;
	void	   *callback_state = vstate->callback_state;
	Relation	rel = info->index;
	bool		attempt_pagedel;
	BlockNumber blkno,
				backtrack_to;
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;

	blkno = scanblkno;

backtrack:

	attempt_pagedel = false;
	backtrack_to = P_NONE;

	/* call vacuum_delay_point while not holding any buffer lock */
	vacuum_delay_point();

	/*
	 * We can't use _bt_getbuf() here because it always applies
	 * _bt_checkpage(), which will barf on an all-zero page. We want to
	 * recycle all-zero pages, not fail.  Also, we want to use a nondefault
	 * buffer access strategy.
	 */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
							 info->strategy);
	_bt_lockbuf(rel, buf, BT_READ);
	page = BufferGetPage(buf);
	opaque = NULL;
	if (!PageIsNew(page))
	{
		_bt_checkpage(rel, buf);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	}

	Assert(blkno <= scanblkno);
	if (blkno != scanblkno)
	{
		/*
		 * We're backtracking.
		 *
		 * We followed a right link to a sibling leaf page (a page that
		 * happens to be from a block located before scanblkno).  The only
		 * case we want to do anything with is a live leaf page having the
		 * current vacuum cycle ID.
		 *
		 * The page had better be in a state that's consistent with what we
		 * expect.  Check for conditions that imply corruption in passing.  It
		 * can't be half-dead because only an interrupted VACUUM process can
		 * leave pages in that state, so we'd definitely have dealt with it
		 * back when the page was the scanblkno page (half-dead pages are
		 * always marked fully deleted by _bt_pagedel(), barring corruption).
		 */
		if (!opaque || !P_ISLEAF(opaque) || P_ISHALFDEAD(opaque))
		{
			Assert(false);
			ereport(LOG,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("right sibling %u of scanblkno %u unexpectedly in an inconsistent state in index \"%s\"",
									 blkno, scanblkno, RelationGetRelationName(rel))));
			_bt_relbuf(rel, buf);
			return;
		}

		/*
		 * We may have already processed the page in an earlier call, when the
		 * page was scanblkno.  This happens when the leaf page split occurred
		 * after the scan began, but before the right sibling page became the
		 * scanblkno.
		 *
		 * Page may also have been deleted by current btvacuumpage() call,
		 * since _bt_pagedel() sometimes deletes the right sibling page of
		 * scanblkno in passing (it does so after we decided where to
		 * backtrack to).  We don't need to process this page as a deleted
		 * page a second time now (in fact, it would be wrong to count it as a
		 * deleted page in the bulk delete statistics a second time).
		 */
		if (opaque->btpo_cycleid != vstate->cycleid || P_ISDELETED(opaque))
		{
			/* Done with current scanblkno (and all lower split pages) */
			_bt_relbuf(rel, buf);
			return;
		}
	}

	if (!opaque || BTPageIsRecyclable(page))
	{
		/* Okay to recycle this page (which could be leaf or internal) */
		RecordFreeIndexPage(rel, blkno);
		stats->pages_deleted++;
		stats->pages_free++;
	}
	else if (P_ISDELETED(opaque))
	{
		/*
		 * Already deleted page (which could be leaf or internal).  Can't
		 * recycle yet.
		 */
		stats->pages_deleted++;
	}
	else if (P_ISHALFDEAD(opaque))
	{
		/* Half-dead leaf page (from interrupted VACUUM) -- finish deleting */
		attempt_pagedel = true;

		/*
		 * _bt_pagedel() will increment both pages_newly_deleted and
		 * pages_deleted stats in all cases (barring corruption)
		 */
	}
	else if (P_ISLEAF(opaque))
	{
		OffsetNumber deletable[MaxIndexTuplesPerPage];
		int			ndeletable;
		BTVacuumPosting updatable[MaxIndexTuplesPerPage];
		int			nupdatable;
		OffsetNumber offnum,
					minoff,
					maxoff;
		int			nhtidsdead,
					nhtidslive;

		/*
		 * Trade in the initial read lock for a super-exclusive write lock on
		 * this page.  We must get such a lock on every leaf page over the
		 * course of the vacuum scan, whether or not it actually contains any
		 * deletable tuples --- see nbtree/README.
		 */
		_bt_upgradelockbufcleanup(rel, buf);

		/*
		 * Check whether we need to backtrack to earlier pages.  What we are
		 * concerned about is a page split that happened since we started the
		 * vacuum scan.  If the split moved tuples on the right half of the
		 * split (i.e. the tuples that sort high) to a block that we already
		 * passed over, then we might have missed the tuples.  We need to
		 * backtrack now.  (Must do this before possibly clearing btpo_cycleid
		 * or deleting scanblkno page below!)
		 */
		if (vstate->cycleid != 0 &&
			opaque->btpo_cycleid == vstate->cycleid &&
			!(opaque->btpo_flags & BTP_SPLIT_END) &&
			!P_RIGHTMOST(opaque) &&
			opaque->btpo_next < scanblkno)
			backtrack_to = opaque->btpo_next;

		/*
		 * When each VACUUM begins, it determines an OldestXmin cutoff value.
		 * Tuples before the cutoff are removed by VACUUM.  Scan over all
		 * items to see which ones need to be deleted according to cutoff
		 * point using callback.
		 */
		ndeletable = 0;
		nupdatable = 0;
		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		nhtidsdead = 0;
		nhtidslive = 0;
		if (callback)
		{
			for (offnum = minoff;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				IndexTuple	itup;

				itup = (IndexTuple) PageGetItem(page,
												PageGetItemId(page, offnum));

				/*
				 * Hot Standby assumes that it's okay that XLOG_BTREE_VACUUM
				 * records do not produce their own conflicts.  This is safe
				 * as long as the callback function only considers whether the
				 * index tuple refers to pre-cutoff heap tuples that were
				 * certainly already pruned away during VACUUM's initial heap
				 * scan by the time we get here. (heapam's XLOG_HEAP2_PRUNE
				 * records produce conflicts using a latestRemovedXid value
				 * for the pointed-to heap tuples, so there is no need to
				 * produce our own conflict now.)
				 *
				 * Backends with snapshots acquired after a VACUUM starts but
				 * before it finishes could have visibility cutoff with a
				 * later xid than VACUUM's OldestXmin cutoff.  These backends
				 * might happen to opportunistically mark some index tuples
				 * LP_DEAD before we reach them, even though they may be after
				 * our cutoff.  We don't try to kill these "extra" index
				 * tuples in _bt_delitems_vacuum().  This keep things simple,
				 * and allows us to always avoid generating our own conflicts.
				 */
				Assert(!BTreeTupleIsPivot(itup));
				if (!BTreeTupleIsPosting(itup))
				{
					/* Regular tuple, standard table TID representation */
					if (callback(&itup->t_tid, callback_state))
					{
						deletable[ndeletable++] = offnum;
						nhtidsdead++;
					}
					else
						nhtidslive++;
				}
				else
				{
					BTVacuumPosting vacposting;
					int			nremaining;

					/* Posting list tuple */
					vacposting = btreevacuumposting(vstate, itup, offnum,
													&nremaining);
					if (vacposting == NULL)
					{
						/*
						 * All table TIDs from the posting tuple remain, so no
						 * delete or update required
						 */
						Assert(nremaining == BTreeTupleGetNPosting(itup));
					}
					else if (nremaining > 0)
					{

						/*
						 * Store metadata about posting list tuple in
						 * updatable array for entire page.  Existing tuple
						 * will be updated during the later call to
						 * _bt_delitems_vacuum().
						 */
						Assert(nremaining < BTreeTupleGetNPosting(itup));
						updatable[nupdatable++] = vacposting;
						nhtidsdead += BTreeTupleGetNPosting(itup) - nremaining;
					}
					else
					{
						/*
						 * All table TIDs from the posting list must be
						 * deleted.  We'll delete the index tuple completely
						 * (no update required).
						 */
						Assert(nremaining == 0);
						deletable[ndeletable++] = offnum;
						nhtidsdead += BTreeTupleGetNPosting(itup);
						pfree(vacposting);
					}

					nhtidslive += nremaining;
				}
			}
		}

		/*
		 * Apply any needed deletes or updates.  We issue just one
		 * _bt_delitems_vacuum() call per page, so as to minimize WAL traffic.
		 */
		if (ndeletable > 0 || nupdatable > 0)
		{
			Assert(nhtidsdead >= ndeletable + nupdatable);
			_bt_delitems_vacuum(rel, buf, deletable, ndeletable, updatable,
								nupdatable);

			stats->tuples_removed += nhtidsdead;
			/* must recompute maxoff */
			maxoff = PageGetMaxOffsetNumber(page);

			/* can't leak memory here */
			for (int i = 0; i < nupdatable; i++)
				pfree(updatable[i]);
		}
		else
		{
			/*
			 * If the leaf page has been split during this vacuum cycle, it
			 * seems worth expending a write to clear btpo_cycleid even if we
			 * don't have any deletions to do.  (If we do, _bt_delitems_vacuum
			 * takes care of this.)  This ensures we won't process the page
			 * again.
			 *
			 * We treat this like a hint-bit update because there's no need to
			 * WAL-log it.
			 */
			Assert(nhtidsdead == 0);
			if (vstate->cycleid != 0 &&
				opaque->btpo_cycleid == vstate->cycleid)
			{
				opaque->btpo_cycleid = 0;
				MarkBufferDirtyHint(buf, true);
			}
		}

		/*
		 * If the leaf page is now empty, try to delete it; else count the
		 * live tuples (live table TIDs in posting lists are counted as
		 * separate live tuples).  We don't delete when backtracking, though,
		 * since that would require teaching _bt_pagedel() about backtracking
		 * (doesn't seem worth adding more complexity to deal with that).
		 *
		 * We don't count the number of live TIDs during cleanup-only calls to
		 * btvacuumscan (i.e. when callback is not set).  We count the number
		 * of index tuples directly instead.  This avoids the expense of
		 * directly examining all of the tuples on each page.  VACUUM will
		 * treat num_index_tuples as an estimate in cleanup-only case, so it
		 * doesn't matter that this underestimates num_index_tuples
		 * significantly in some cases.
		 */
		if (minoff > maxoff)
			attempt_pagedel = (blkno == scanblkno);
		else if (callback)
			stats->num_index_tuples += nhtidslive;
		else
			stats->num_index_tuples += maxoff - minoff + 1;

		Assert(!attempt_pagedel || nhtidslive == 0);
	}

	if (attempt_pagedel)
	{
		MemoryContext oldcontext;

		/* Run pagedel in a temp context to avoid memory leakage */
		MemoryContextReset(vstate->pagedelcontext);
		oldcontext = MemoryContextSwitchTo(vstate->pagedelcontext);

		/*
		 * _bt_pagedel maintains the bulk delete stats on our behalf;
		 * pages_newly_deleted and pages_deleted are likely to be incremented
		 * during call
		 */
		Assert(blkno == scanblkno);
		_bt_pagedel(rel, buf, vstate);

		MemoryContextSwitchTo(oldcontext);
		/* pagedel released buffer, so we shouldn't */
	}
	else
		_bt_relbuf(rel, buf);

	if (backtrack_to != P_NONE)
	{
		blkno = backtrack_to;
		goto backtrack;
	}
}

/*
 * btreevacuumposting --- determine TIDs still needed in posting list
 *
 * Returns metadata describing how to build replacement tuple without the TIDs
 * that VACUUM needs to delete.  Returned value is NULL in the common case
 * where no changes are needed to caller's posting list tuple (we avoid
 * allocating memory here as an optimization).
 *
 * The number of TIDs that should remain in the posting list tuple is set for
 * caller in *nremaining.
 */
static BTVacuumPosting
btreevacuumposting(BTVacState *vstate, IndexTuple posting,
				   OffsetNumber updatedoffset, int *nremaining)
{
	int			live = 0;
	int			nitem = BTreeTupleGetNPosting(posting);
	ItemPointer items = BTreeTupleGetPosting(posting);
	BTVacuumPosting vacposting = NULL;

	for (int i = 0; i < nitem; i++)
	{
		if (!vstate->callback(items + i, vstate->callback_state))
		{
			/* Live table TID */
			live++;
		}
		else if (vacposting == NULL)
		{
			/*
			 * First dead table TID encountered.
			 *
			 * It's now clear that we need to delete one or more dead table
			 * TIDs, so start maintaining metadata describing how to update
			 * existing posting list tuple.
			 */
			vacposting = palloc(offsetof(BTVacuumPostingData, deletetids) +
								nitem * sizeof(uint16));

			vacposting->itup = posting;
			vacposting->updatedoffset = updatedoffset;
			vacposting->ndeletedtids = 0;
			vacposting->deletetids[vacposting->ndeletedtids++] = i;
		}
		else
		{
			/* Second or subsequent dead table TID */
			vacposting->deletetids[vacposting->ndeletedtids++] = i;
		}
	}

	*nremaining = live;
	return vacposting;
}

/*
 *	btcanreturn() -- Check whether btree indexes support index-only scans.
 *
 * btrees always do, so this is trivial.
 */
bool
btcanreturn(Relation index, int attno)
{
	return true;
}
