#include "btr0cur.h"

#include "page0page.h"
#include "rem0rec.h"
#include "rem0cmp.h"
#include "btr0btr.h"
#include "btr0sea.h"
#include "row0upd.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "srv0srv.h"
#include "ibuf0ibuf.h"
#include "lock0lock.h"

ibool	btr_cur_print_record_ops = FALSE;

ulint	btr_cur_rnd = 0;

ulint	btr_cur_n_non_sea = 0;
ulint	btr_cur_n_sea = 0;
ulint	btr_cur_n_non_sea_old = 0;
ulint	btr_cur_n_sea_old = 0;


/*页重组因子*/
#define BTR_CUR_PAGE_REORGANIZE_LIMIT	(UNIV_PAGE_SIZE / 32)

#define BTR_KEY_VAL_ESTIMATE_N_PAGES	8

/*BLOB列的头结构*/
#define BTR_BLOB_HDR_PART_LEN			0	/*blob在对应页中的长度*/
#define BTR_BLOB_HDR_NEXT_PAGE_NO		4	/*下一个存有blob数据的page no*/
#define BTR_BLOB_HDR_SIZE				8	


static void			btr_cur_add_path_info(btr_cur_t* cursor, ulint height, ulint root_height);
static void			btr_rec_free_updated_extern_fields(dict_index_t* index, rec_t* rec, upd_t* update, ibool do_not_free_inherited, mtr_t* mtr);
static ulint		btr_rec_get_externally_stored_len(rec_t* rec);

/*对page做互斥latch并发*/
static void btr_cur_latch_leaves(dict_tree_t* tree, page_t* page, ulint space, ulint page_no, ulint latch_mode, btr_cur_t* cursor, mtr_t* mtr)
{
	ulint	left_page_no;
	ulint	right_page_no;

	ut_ad(tree && page && mtr);

	if(latch_mode == BTR_SEARCH_LEAF) /*查找时，可以直接获取一个S-LATCH,*/
		btr_page_get(space, page_no, RW_S_LATCH, mtr);
	else if(latch_mode == BTR_MODIFY_LEAF) /*叶子节点修改，直接对叶子节点x-latch即可*/
		btr_page_get(space, page_no, RW_X_LATCH, mtr);
	else if(latch_mode == BTR_MODIFY_TREE){ /*分别会获取前后两页和自己的x-latch*/
		left_page_no = btr_page_get_prev(page, mtr);
		if(left_pge_no != FIL_NULL) /*对前一页获取一个X-LATCH*/
			btr_page_get(space, left_page_no, RW_X_LATCH, mtr);

		btr_page_get(space, page_no, RW_X_LATCH, mtr);

		/*对后一个页也获取一个X-LATCH*/
		right_page_no = btr_page_get_next(page, mtr);
		if(right_page_no != FIL_NULL)
			btr_page_get(space, right_page_no, RW_X_LATCH, mtr);
	}
	else if(latch_mode == BTR_SEARCH_PREV){ /*搜索前一页，对前一页加上一个s-latch*/
		left_page_no = btr_page_get_prev(page, mtr);
		if(left_page_no != FIL_NULL)
			cursor->left_page = btr_page_get(space, left_page_no, RW_S_LATCH, mtr);

		btr_page_get(space, page_no, RW_S_LATCH, mtr);
	}
	else if(latch_mode == BTR_MODIFY_PREV){ /*更改前一页，对前一页加上一个x-latch*/
		left_page_no = btr_page_get_prev(page, mtr);
		if(left_page_no != FIL_NULL)
			cursor->left_page = btr_page_get(space, left_page_no, RW_X_LATCH, mtr);

		btr_page_get(space, page_no, RW_X_LATCH, mtr);
	}
	else
		ut_error;
}

void btr_cur_search_to_nth_level(dict_index_t* index, ulint level, dtuple_t* tuple, ulint latch_mode,
	btr_cur_t* cursor, ulint has_search_latch, mtr_t* mtr)
{
	dict_tree_t*	tree;
	page_cur_t*	page_cursor;
	page_t*		page;
	page_t*		guess;
	rec_t*		node_ptr;
	ulint		page_no;
	ulint		space;
	ulint		up_match;
	ulint		up_bytes;
	ulint		low_match;
	ulint 		low_bytes;
	ulint		height;
	ulint		savepoint;
	ulint		rw_latch;
	ulint		page_mode;
	ulint		insert_planned;
	ulint		buf_mode;
	ulint		estimate;
	ulint		ignore_sec_unique;
	ulint		root_height;

	btr_search_t* info;

	ut_ad(level == 0 || mode == PAGE_CUR_LE);
	ut_ad(dict_tree_check_search_tuple(index->tree, tuple));
	ut_ad(!(index->type & DICT_IBUF) || ibuf_inside());
	ut_ad(dtuple_check_typed(tuple));

	insert_planned = latch_mode & BTR_INSERT;
	estimate = latch_mode & BTR_ESTIMATE;			/*预估计算的动作*/
	ignore_sec_unique = latch_mode & BTR_IGNORE_SEC_UNIQUE;
	latch_mode = latch_mode & ~(BTR_INSERT | BTR_ESTIMATE | BTR_IGNORE_SEC_UNIQUE);

	ut_ad(!insert_planned || mode == PAGE_CUR_LE);

	cursor->flag = BTR_CUR_BINARY;
	cursor->index = index;

	/*在自适应HASH表中查找*/
	info = btr_search_get_info(index);
	guess = info->root_guess;

#ifdef UNIV_SEARCH_PERF_STAT
	info->n_searches++;
#endif

	/*在自适应hash中找到了对应的记录*/
	if (btr_search_latch.writer == RW_LOCK_NOT_LOCKED
		&& latch_mode <= BTR_MODIFY_LEAF && info->last_hash_succ
		&& !estimate
		&& btr_search_guess_on_hash(index, info, tuple, mode, latch_mode, cursor, has_search_latch, mtr)) {

			ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_GE);
			ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
			ut_ad(cursor->low_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
			btr_cur_n_sea++;

			return;
	}

	btr_cur_n_sea ++;

	if(has_search_latch)
		rw_lock_s_unlock(&btr_search_latch);

	/*获得mtr 的保存数据长度*/
	savepoint = mtr_set_savepoint(mtr);

	tree = index->tree;

	/*获取一个x-latch,更改有可能会更改索引*/
	if(latch_mode == BTR_MODIFY_TREE)
		mtr_x_lock(dict_tree_get_lock(tree), mtr);
	else if(latch_mode == BTR_CONT_MODIFY_TREE) 
		ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	else /*对tree 索引获取一个s-latch*/
		mtr_s_lock(dict_tree_get_lock(tree), mtr);

	page_cursor = btr_cur_get_page_cur(cursor);
	space = dict_tree_get_space(tree);
	page_no = dict_tree_get_page(tree);

	up_match = 0;
	up_bytes = 0;
	low_match = 0;
	low_bytes = 0;

	height = ULINT_UNDEFINED;
	rw_latch = RW_NO_LATCH;
	buf_mode = BUF_GET;

	/*确定page记录的匹配模式*/
	if (mode == PAGE_CUR_GE)
		page_mode = PAGE_CUR_L;
	else if (mode == PAGE_CUR_G)
		page_mode = PAGE_CUR_LE;
	else if (mode == PAGE_CUR_LE)
		page_mode = PAGE_CUR_LE;
	else {
		ut_ad(mode == PAGE_CUR_L);
		page_mode = PAGE_CUR_L;
	}

	for(;;){
		if(height == 0 && latch_mode <= BTR_MODIFY_LEAF){
			rw_latch = latch_mode;
			/*尝试将page插入到ibuffer当中*/
			if(insert_planned && ibuf_should_try(index, ignore_sec_unique))
				buf_mode = BUF_GET_IF_IN_POOL;
		}

retry_page_get:
		page = buf_page_get_gen(space, page_no, rw_latch, guess, buf_mode, IB__FILE__, __LINE__, mtr);
		if(page == NULL){
			ut_ad(buf_mode == BUF_GET_IF_IN_POOL);
			ut_ad(insert_planned);
			ut_ad(cursor->thr);

			/*page不能插入到insert buffer中,将会重试,知道page插入到ibuf中*/
			if(ibuf_should_try(index, ignore_sec_unique) && ibuf_insert(tuple, index, space, page_no, cursor->thr)){
				cursor->flag = BTR_CUR_INSERT_TO_IBUF;
				return ;
			}

			buf_mode = BUF_GET;
			goto retry_page_get;
		}

		ut_ad(0 == ut_dulint_cmp(tree->id, btr_page_get_index_id(page)));
		if(height == ULINT_UNDEFINED){
			height = btr_page_get_level(page, mtr);
			root_height = height;
			cursor->tree_height = root_height + 1;

			if(page != guess)
				info->root_guess = page;
		}

		if(height == 0){
			if(rw_latch == RW_NO_LATCH)
				btr_cur_latch_leaves(tree, page, space, page_no, latch_mode, cursor, mtr);
			/*释放savepoint以下的mtr latch*/
			if(latch_mode != BTR_MODIFY_TREE && latch_mode != BTR_CONT_MODIFY_TREE)
				mtr_release_s_latch_at_savepoint(mtr, savepoint, dict_tree_get_lock(tree));

			page_mode = mode;
		}
		/*在页中进行二分查找对应的记录,这个只是找page node ptr记录*/
		page_cur_search_with_match(page, tuple, page_mode, &up_match, &up_bytes, &low_match, &low_bytes, page_cursor);
		if(estimate)
			btr_cur_add_path_info(cursor, height, root_height);

		if(level == height){ /*已经找到对应的层了，不需要深入更低层上*/
			if(level > 0)
				btr_page_get(space, page_no, RW_X_LATCH, mtr);
			break;
		}

		ut_ad(height > 0);

		height --;
		guess = NULL;

		node_ptr = page_cur_get_rec(page_cursor);
		/*获取孩子节点的page no*/
		page_no = btr_node_ptr_get_child_page_no(node_ptr);
	}

	if(level == 0){
		cursor->low_match = low_match;
		cursor->low_bytes = low_bytes;
		cursor->up_match = up_match;
		cursor->up_bytes = up_bytes;
		/*更新自适应HASH索引*/
		btr_search_info_update(index, cursor);

		ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_GE);
		ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
		ut_ad(cursor->low_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
	}

	if(has_search_latch)
		rw_lock_s_lock(&btr_search_latch);
}

/*将btree cursor定位到index索引范围的开始或者末尾，from_left = TRUE，表示定位到最前面*/
void btr_cur_open_at_index_side(ibool from_left, dict_index_t* index, ulint latch_mode, btr_cur_t* cursor, mtr_t* mtr)
{
	page_cur_t*	page_cursor;
	dict_tree_t*	tree;
	page_t*		page;
	ulint		page_no;
	ulint		space;
	ulint		height;
	ulint		root_height;
	rec_t*		node_ptr;
	ulint		estimate;
	ulint       savepoint;

	estimate = latch_mode & BTR_ESTIMATE;
	latch_mode = latch_mode & ~BTR_ESTIMATE;

	tree = index->tree;

	savepoint = mtr_set_savepoint(mtr);
	if(latch_mode == BTR_MODIFY_TREE)
		mtr_x_lock(dict_tree_get_lock(tree), mtr);
	else
		mtr_s_lock(dict_tree_get_lock(tree), mtr);

	page_cursor = btr_cur_get_page_cur(cursor);
	cursor->index = index;

	space = dict_tree_get_space(tree);
	page_no = dict_tree_get_page(tree);

	height = ULINT_UNDEFINED;
	for(;;){
		page = buf_page_get_gen(space, page_no, RW_NO_LATCH, NULL,
			BUF_GET, IB__FILE__, __LINE__, mtr);

		ut_ad(0 == ut_dulint_cmp(tree->id, btr_page_get_index_id(page)));

		if(height == ULINT_UNDEFINED){
			height = btr_page_get_level(page, mtr);
			root_height = height;
		}

		if(height == 0){
			btr_cur_latch_leaves(tree, page, space, page_no, latch_mode, cursor, mtr);

			if(latch_mode != BTR_MODIFY_TREE && latch_mode != BTR_CONT_MODIFY_TREE)
				mtr_release_s_latch_at_savepoint(mtr, savepoint, dict_tree_get_lock(tree));
		}

		if(from_left) /*从页第一条记录*/
			page_cur_set_before_first(page, page_cursor);
		else /*从页的最后一条记录开始*/
			page_cur_set_after_last(page, page_cursor);

		if(height == 0){
			if(estimate)
				btr_cur_add_path_info(cursor, height, root_height);
			break;
		}

		ut_ad(height > 0);

		if(from_left)
			page_cur_move_to_next(page_cursor);
		else
			page_cur_move_to_prev(page_cursor);

		if(estimate)
			btr_cur_add_path_info(cursor, height, root_height);

		height --;
		node_ptr = page_cur_get_rec(page_cursor);

		page_no = btr_node_ptr_get_child_page_no(node_ptr);
	}
}

/*在btree index的管辖范围，随机定位到一个位置*/
void btr_cur_open_at_rnd_pos(dict_index_t* index, ulint latch_mode, btr_cur_t* cursor, mtr_t* mtr)
{
	page_cur_t*	page_cursor;
	dict_tree_t*	tree;
	page_t*		page;
	ulint		page_no;
	ulint		space;
	ulint		height;
	rec_t*		node_ptr;

	tree = index->tree;
	if(latch_mode == BTR_MODIFY_TREE)
		mtr_x_lock(dict_tree_get_lock(tree), mtr);
	else
		mtr_s_lock(dict_tree_get_lock(tree), mtr);

	page_cursor = btr_cur_get_page_cur(cursor);
	cursor->index = index;

	space = dict_tree_get_space(tree);
	page_no = dict_tree_get_page(tree);

	height = ULINT_UNDEFINED;
	for(;;){
		page = buf_page_get_gen(space, page_no, RW_NO_LATCH, NULL, BUF_GET, IB__FILE__, __LINE__, mtr);
		ut_ad(0 == ut_dulint_cmp(tree->id, btr_page_get_index_id(page)));

		if(height == ULINT_UNDEFINED)
			height = btr_page_get_level(page, mtr);

		if(height == 0)
			btr_cur_latch_leaves(tree, page, space, page_no, latch_mode, cursor, mtr);
		/*随机定位一条记录，并将page cursor指向它*/
		page_cur_open_on_rnd_user_rec(page, page_cursor);	
		if(height == 0)
			break;

		ut_ad(height > 0);
		height --;

		node_ptr = page_cur_get_rec(page_cursor);
		page_no = btr_node_ptr_get_child_page_no(node_ptr);
	}
}

static rec_t* btr_cur_insert_if_possible(btr_cur_t* cursor, dtuple_t* tuple, ibool* reorg, mtr_t* mtr)
{
	page_cur_t*	page_cursor;
	page_t*		page;
	rec_t*		rec;

	ut_ad(dtuple_check_typed(tuple));

	*reorg = FALSE;

	page = btr_cur_get_page(cursor);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	page_cursor = btr_cur_get_page_cur(cursor);

	/*将tuple插入到page中*/
	rec = page_cur_tuple_insert(page_cursor, tuple, mtr);
	if(rec == NULL){
		/*可能空间不够或者排序不对，进行页重组,有可能插入记录的空间无法在page上分配(没有空闲的rec空间，删除的单条记录 < tuple所需要的空间)*/
		btr_page_reorganize(page, mtr);
		*reorg = TRUE;
		/*重新定位page游标*/
		page_cur_search(page, tuple, PAGE_CUR_GE, page_cursor);

		rec = page_cur_tuple_insert(page_cursor, tuple, mtr);
	}

	return rec;
}

/*为插入的记录添加一个事务锁和回滚日志*/
UNIV_INLINE ulint btr_cur_ins_lock_and_undo(ulint flags, btr_cur_t* cursor, dtuple_t* entry, que_thr_t* thr, ibool* inherit)
{
	dict_index_t*	index;
	ulint		err;
	rec_t*		rec;
	dulint		roll_ptr;

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	/*添加一个事务锁，如果有对应的行锁，重复利用*/
	err = lock_rec_insert_check_and_lock(flags, rec, index, thr, inherit);
	if(err != DB_SUCCESS) /*添加锁失败*/
		return err;

	if(index->type & DICT_CLUSTERED && !(index->type & DICT_IBUF)){
		err = trx_undo_report_row_operation(flags, TRX_UNDO_INSERT_OP, thr, index, entry,
							NULL, 0, NULL, &roll_ptr);

		if(err != DB_SUCCESS)
			return err;

		if(!(flags & BTR_KEEP_SYS_FLAG))
			row_upd_index_entry_sys_field(entry, index, DATA_ROLL_PTR, roll_ptr);
	}

	return DB_SUCCESS;
}

/*尝试以乐观锁方式插入记录*/
ulint btr_cur_optimistic_insert(ulint flags, btr_cur_t* cursor, dtuple_t* entry, rec_t** rec, big_rec_t** big_rec, que_thr_t* thr, mtr_t* mtr)
{
	big_rec_t*	big_rec_vec	= NULL;
	dict_index_t*	index;
	page_cur_t*	page_cursor;
	page_t*		page;
	ulint		max_size;
	rec_t*		dummy_rec;
	ulint		level;
	ibool		reorg;
	ibool		inherit;
	ulint		rec_size;
	ulint		data_size;
	ulint		extra_size;
	ulint		type;
	ulint		err;

	*big_rec = NULL;

	page = btr_cur_get_page(cursor);
	index = cursor->index;

	if(!dtuple_check_typed_no_assert(entry))
		fprintf(stderr, "InnoDB: Error in a tuple to insert into table %lu index %s\n",
			index->table_name, index->name);

	if (btr_cur_print_record_ops && thr) {
		printf("Trx with id %lu %lu going to insert to table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		dtuple_print(entry);
	}

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*page可以容纳的最大记录空间*/
	max_size = page_get_max_insert_size_after_reorganize(page, 1);
	level = btr_page_get_level(page, mtr); /*获得page所处的层*/

calculate_sizes_again:
	/*获得tuple存储需要的空间大小*/
	data_size = dtuple_get_data_size(entry);
	/*获得rec header需要的长度*/
	extra_size = rec_get_converted_extra_size(data_size, dtuple_get_n_fields(entry));
	rec_size = data_size + extra_size;

	/*记录超出最大的存储范围，将其转化为big_rec*/
	if((rec_size >= page_get_free_space_of_empty() / 2) || rec_size >= REC_MAX_DATA_SIZE){
		big_rec_vec = dtuple_convert_big_rec(index, entry, NULL, 0);

		/*转化失败,记录实在太大,可能有太多的短列？*/
		if(big_rec_vec == NULL)
			return DB_TOO_BIG_RECORD;

		goto calculate_sizes_again;
	}

	type = index->type;

	/*聚集索引，在叶子节点可以进行左右分裂,而且BTREE树的空间不够？？不能存入，将转化为big_rec的操作回滚*/
	if ((type & DICT_CLUSTERED)
		&& (dict_tree_get_space_reserve(index->tree) + rec_size > max_size)
		&& (page_get_n_recs(page) >= 2)
		&& (0 == level)
		&& (btr_page_get_split_rec_to_right(cursor, &dummy_rec)
		|| btr_page_get_split_rec_to_left(cursor, &dummy_rec))){
			if(big_rec_vec) /*将tuple转化成big_rec*/
				dtuple_convert_back_big_rec(index, entry, big_rec_vec);

			return DB_FAIL;
	}

	if (!(((max_size >= rec_size) && (max_size >= BTR_CUR_PAGE_REORGANIZE_LIMIT))
		|| page_get_max_insert_size(page, 1) >= rec_size || page_get_n_recs(page) <= 1)) {
			if(big_rec_vec)
				dtuple_convert_back_big_rec(index, entry, big_rec_vec);

			return DB_FAIL;
	}

	err = btr_cur_ins_lock_and_undo(flags, cursor, entry, thr, &inherit);
	if(err != DB_SUCCESS){ /*事务加锁不成功，回滚big_rec*/
		if(big_rec_vec)
			dtuple_convert_back_big_rec(index, entry, big_rec_vec);

		return err;
	}

	page_cursor = btr_cur_get_page_cur(cursor);
	reorg = FALSE;

	/*将tuple插入到page当中*/
	*rec = page_cur_insert_rec_low(page_cursor, entry, data_size, NULL, mtr);
	if(*rec == NULL){ /*插入失败，进行page重组*/
		btr_page_reorganize(page, mtr);

		ut_ad(page_get_max_insert_size(page, 1) == max_size);
		reorg = TRUE;

		page_cur_search(page, entry, PAGE_CUR_LE, page_cursor);
		*rec = page_cur_tuple_insert(page_cursor, entry, mtr);
		if(*rec == NULL){ /*重组后还是失败，打印错误信息*/
			char* err_buf = mem_alloc(1000);

			dtuple_sprintf(err_buf, 900, entry);

			fprintf(stderr, "InnoDB: Error: cannot insert tuple %s to index %s of table %s\n"
				"InnoDB: max insert size %lu\n", err_buf, index->name, index->table->name, max_size);

			mem_free(err_buf);
		}

		ut_a(*rec);
	}

	/*更新HASH索引*/
	if(!reorg && level == 0 && cursor->flag == BTR_CUR_HASH)
		btr_search_update_hash_node_on_insert(cursor);
	else
		btr_search_update_hash_on_insert(cursor);

	/*继承后一行事务锁(GAP方式)*/
	if(!(flags & BTR_NO_LOCKING_FLAG) && inherit)
		lock_update_insert(*rec);

	/*非聚集索引插入*/
	if(!(type & DICT_CLUSTERED))
		ibuf_update_free_bits_if_full(cursor->index, page, max_size,
		rec_size + PAGE_DIR_SLOT_SIZE);

	*big_rec = big_rec_vec;

	return DB_SUCCESS;
}

/*以悲观锁执行记录的插入,悲观方式是表空间不够，需要扩大表空间*/
ulint btr_cur_pessimistic_insert(ulint flags, btr_cur_t* cursor, dtuple_t* entry, rec_t** rec, big_rec_t** big_rec, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index = cursor->index;
	big_rec_t*	big_rec_vec	= NULL;
	page_t*		page;
	ulint		err;
	ibool		dummy_inh;
	ibool		success;
	ulint		n_extents = 0;

	*big_rec = NULL;
	page = btr_cur_get_page(cursor);

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(btr_cur_get_tree(cursor)), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	cursor->flag = BTR_CUR_BINARY;

	err = btr_cur_optimistic_insert(flags, cursor, entry, rec, big_rec, thr, mtr);
	if(err != DB_FAIL) /*乐观锁方式插入成功，直接返回*/
		return err;

	err = btr_cur_ins_lock_and_undo(flags, cursor, entry, thr, &dummy_inh);
	if(err != DB_SUCCESS)
		return err;

	if(!(flags && BTR_NO_UNDO_LOG_FLAG)){
		n_extents = cursor->tree_height / 16 + 3;

		/*扩大file space表空间*/
		success = fsp_reserve_free_extents(index->space, n_extents, FSP_NORMAL, mtr);
		if(!success){ /*超出表空间范围*/
			err =  DB_OUT_OF_FILE_SPACE;
			return err;
		}
	}

	/*单个页无法存储entry记录,作为大记录存储*/
	if(rec_get_converted_size(entry) >= page_get_free_space_of_empty() / 2 || rec_get_converted_size(entry) >= REC_MAX_DATA_SIZE){
		 big_rec_vec = dtuple_convert_big_rec(index, entry, NULL, 0);
		 if(big_rec_vec == NULL)
			 return DB_TOO_BIG_RECORD;
	}

	if (dict_tree_get_page(index->tree) == buf_frame_get_page_no(page))
			*rec = btr_root_raise_and_insert(cursor, entry, mtr);
	else
		*rec = btr_page_split_and_insert(cursor, entry, mtr);
	
	btr_cur_position(index, page_rec_get_prev(*rec), cursor);

	/*更新自适应HASH*/
	btr_search_update_hash_on_insert(cursor);

	/*新插入行对gap行锁的继承*/
	if(!(flags & BTR_NO_LOCKING_FLAG))
		lock_update_insert(*rec);

	err = DB_SUCCESS;

	if(n_extents > 0)
		fil_space_release_free_extents(index->space, n_extents);

	*big_rec = big_rec_vec;

	return err;
}

/*按聚集索引修改记录，进行事务锁请求*/
UNIV_INLINE ulint btr_cur_upd_lock_and_undo(ulint flags, btr_cur_t* cursor, upd_t* update, ulint cmpl_info, que_thr_t* thr, dulint roll_ptr)
{
	dict_index_t*	index;
	rec_t*			rec;
	ulint			err;

	ut_ad(cursor && update && thr && roll_ptr);
	ut_ad(cursor->index->type & DICT_CLUSTERED);

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	err =DB_SUCCESS;

	if(!(flags & BTR_NO_LOCKING_FLAG)){
		/*检查修改记录时，记录聚集索引上是否有锁（包括显式锁和隐式锁）*/
		err = lock_clust_rec_modify_check_and_lock(flags, rec, index, thr);
		if(err != DB_SUCCESS)
			return err;
	}

	err = trx_undo_report_row_operation(flags, TRX_UNDO_MODIFY_OP, thr, index, NULL, update,
		cmpl_info, rec, roll_ptr);

	return err;
}

/*为update record增加一条redo log*/
UNIV_INLINE void btr_cur_update_in_place_log(ulint flags, rec_t* rec, dict_index_t* index, 
	upd_t* update, trx_t* trx, dulint roll_ptr, mtr_t* mtr)
{
	byte* log_ptr;

	log_ptr = mlog_open(mtr, 30 + MLOG_BUF_MARGIN);
	log_ptr = mlog_write_initial_log_record_fast(rec, MLOG_REC_UPDATE_IN_PLACE, log_ptr, mtr);
	
	mach_write_to_1(log_ptr, flags);
	log_ptr++;
	
	log_ptr = row_upd_write_sys_vals_to_log(index, trx, roll_ptr, log_ptr, mtr);

	mach_write_to_2(log_ptr, rec - buf_frame_align(rec));
	log_ptr += 2;

	row_upd_index_write_log(update, log_ptr, mtr);
}

/*解析一条修改记录的redo log并进行重演*/
byte* btr_cur_parse_update_in_place(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ulint	flags;
	rec_t*	rec;
	upd_t*	update;
	ulint	pos;
	dulint	trx_id;
	dulint	roll_ptr;
	ulint	rec_offset;
	mem_heap_t* heap;

	if(end_ptr < ptr + 1)
		return NULL;

	flags = mach_read_from_1(ptr);
	ptr++;

	ptr = row_upd_parse_sys_vals(ptr, end_ptr, &pos, &trx_id, &roll_ptr);

	if(ptr == NULL)
		return NULL;

	if(end_ptr < ptr + 2)
		return NULL;

	/*从redo log中读取修改记录的偏移*/
	rec_offset = mach_read_from_2(ptr);
	ptr += 2;

	/*从redo log中读取修改的内容*/
	heap = mem_heap_create(256);
	ptr = row_upd_index_parse(ptr, end_ptr, heap, &update);
	if(ptr == NULL){
		mem_heap_free(heap);
		return NULL;
	}

	if(page == NULL){
		mem_heap_free(heap);
		return NULL;
	}

	rec = page + rec_offset;
	if(!(flags & BTR_KEEP_SYS_FLAG))
		row_upd_rec_sys_fields_in_recovery(rec, pos, trx_id, roll_ptr);

	/*进行记录修改*/
	row_upd_rec_in_place(heap, update);

	mem_heap_free(heap);

	return ptr;
}
/*通过二级索引修改对应的记录*/
ulint btr_cur_update_sec_rec_in_place(btr_cur_t* cursor, upd_t* update, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index = cursor->index;
	dict_index_t*	clust_index;
	ulint		err;
	rec_t*		rec;
	dulint		roll_ptr = ut_dulint_zero;
	trx_t*		trx	= thr_get_trx(thr);

	ut_ad(0 == (index->type & DICT_CLUSTERED));

	rec = btr_cur_get_rec(cursor);

	if(btr_cur_print_record_ops && thr){
		printf("Trx with id %lu %lu going to update table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),ut_dulint_get_low(thr_get_trx(thr)->id), index->table_name, index->name);

		rec_print(rec);
	}

	/*通过二级索引获得行上的锁*/
	err = lock_sec_rec_modify_check_and_lock(0, rec, index, thr);
	if(err != DB_SUCCESS)
		return err;

	/*删除自适应HASH对应关系*/
	btr_search_update_hash_on_delete(cursor);
	/*对记录进行修改*/
	row_upd_rec_in_place(rec, update);

	/*通过二级索引找到聚集索引*/
	clust_index = dict_table_get_first_index(index->table);
	/*通过聚集索引添加一条修改记录的redo log*/
	btr_cur_update_in_place_log(BTR_KEEP_SYS_FLAG, rec, clust_index, update, trx, roll_ptr, mtr);
	
	return DB_SUCCESS;
}

ulint btr_cur_update_in_place(ulint flags, btr_cur_t* cursor, upd_t* update, ulint cmpl_info, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index;
	buf_block_t*	block;
	ulint		err;
	rec_t*		rec;
	dulint		roll_ptr;
	trx_t*		trx;
	ibool		was_delete_marked;

	ut_ad(cursor->index->type & DICT_CLUSTERED);

	/*获得对应的记录、索引、事务*/
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;
	trx = thr_get_trx(thr);

	if(btr_cur_print_record_ops && thr){
		printf("Trx with id %lu %lu going to update table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		rec_print(rec);
	}

	/*获得事务的行锁*/
	err = btr_cur_upd_lock_and_undo(flags, cursor, update, cmpl_info, thr, &roll_ptr);
	if(err != DB_SUCCESS)
		return err;

	block = buf_block_align(rec);
	if(block->is_hashed){
		if (row_upd_changes_ord_field_binary(NULL, index, update)) 
			btr_search_update_hash_on_delete(cursor);

		rw_lock_x_lock(&btr_search_latch);
	}

	if(!(flags & BTR_KEEP_SYS_FLAG))
		row_upd_rec_sys_fields(rec, index, trx, roll_ptr);

	was_delete_marked = rec_get_deleted_flag(rec);
	/*进行记录更新*/
	row_upd_rec_in_place(rec, update);

	if(block->is_hashed)
		rw_lock_x_unlock(&btr_search_latch);

	btr_cur_update_in_place_log(flags, rec, index, update, trx, roll_ptr, mtr);

	/*由删除状态变成未删除状态*/
	if(was_delete_marked && !rec_get_deleted_flag(rec))
		btr_cur_unmark_extern_fields(rec, mtr);

	return DB_SUCCESS;
}

/*乐观方式更新一条记录，先尝试在原来的记录位置直接更新，如果不能就会将原来的记录删除，将更新的内容组成一条记录插入到对应的page中*/
ulint btr_cur_optimistic_update(ulint flags, btr_cur_t* cursor, upd_t* update, ulint cmpl_info, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index;
	page_cur_t*	page_cursor;
	ulint		err;
	page_t*		page;
	rec_t*		rec;
	ulint		max_size;
	ulint		new_rec_size;
	ulint		old_rec_size;
	dtuple_t*	new_entry;
	dulint		roll_ptr;
	trx_t*		trx;
	mem_heap_t*	heap;
	ibool		reorganized	= FALSE;
	ulint		i;

	ut_ad((cursor->index)->type & DICT_CLUSTERED);

	page = btr_cur_get_page(cursor);
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	if (btr_cur_print_record_ops && thr) {
		printf("Trx with id %lu %lu going to update table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		rec_print(rec);
	}

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_BUF_FIX));

	/*不能改变列的大小，直接在原来的记录位置做更新*/
	if(!row_upd_changes_field_size(rec, index, update))
		return btr_cur_update_in_place(flags, cursor, update, cmpl_info, thr, mtr);

	/*判断列是否溢出*/
	for(i = 0; i < upd_get_n_fields(update); i++){
		if(upd_get_nth_field(update, i)->extern_storage)
			return DB_OVERFLOW;
	}

	/*列已是两个字节表示长度*/
	if(rec_contains_externally_stored_field(btr_cur_get_rec(cursor)))
		return DB_OVERFLOW;

	page_cursor = btr_cur_get_page_cur(cursor);
	heap = mem_heap_create(1024);

	/*在内存中构建一个新记录的存储对象记录*/
	new_entry = row_rec_to_index_entry(ROW_COPY_DATA, index, rec, heap);
	row_upd_clust_index_replace_new_col_vals(new_entry, update);

	old_rec_size = rec_get_size(rec);
	new_rec_size = rec_get_converted_size(new_entry);

	/*跟新的记录大小已经超过页能存储的大小*/
	if(new_rec_size >= page_get_free_space_of_empty() / 2){
		mem_heap_free(heap);
		return DB_OVERFLOW;
	}
	/*计算页重组后可以存储的最大空间*/
	max_size = old_rec_size + page_get_max_insert_size_after_reorganize(page, 1);
	if(page_get_data_size(page) - old_rec_size + new_rec_size < BTR_CUR_PAGE_COMPRESS_LIMIT){
		/*记录更新后会触发btree的合并填充，不做直接更新*/
		mem_heap_free(heap);
		return DB_UNDERFLOW;
	}

	/*能存储的空间小于合并的空间阈值或者不能存下新的记录,并且page的空间不只1条记录*/
	if(!((max_size >= BTR_CUR_PAGE_REORGANIZE_LIMIT && max_size >= new_rec_size) || page_get_n_recs(page) <= 1)){
		mem_heap_free(heap);
		return DB_OVERFLOW;
	}

	/*不能对记录加事务锁*/
	err = btr_cur_upd_lock_and_undo(flags, cursor, update, cmpl_info, thr, &roll_ptr);
	if(err != DB_SUCCESS){
		mem_heap_free(heap);
		return err;
	}

	/*将记录的行锁全部转移到infimum,应该是临时存储在这个地方*/
	lock_rec_store_on_page_infimum(rec);

	btr_search_update_hash_on_delete(cursor);
	/*将page游标对应的记录删除*/
	page_cur_delete_rec(page_cursor, mtr);
	/*游标移到前面一条记录上*/
	page_cur_move_to_prev(page_cursor);

	trx = thr_get_trx(thr);

	if(!(flags & BTR_KEEP_SYS_FLAG)){
		row_upd_index_entry_sys_field(new_entry, index, DATA_ROLL_PTR, roll_ptr);
		row_upd_index_entry_sys_field(new_entry, index, DATA_TRX_ID, trx->id);
	}
	/*将新构建的tuple记录插入到页中*/
	rec = btr_cur_insert_if_possible(cursor, new_entry, &reorganized, mtr);
	ut_a(rec);

	if(!rec_get_deleted_flag(rec))
		btr_cur_unmark_extern_fields(rec, mtr);

	/*将缓存在infimum上的锁恢复到新插入的记录上*/
	lock_rec_restore_from_page_infimum(rec, page);

	page_cur_move_to_next(page_cursor);

	mem_heap_free(heap);

	return DB_SUCCESS;
}

static void btr_cur_pess_upd_restore_supremum(rec_t* rec, mtr_t* mtr)
{
	page_t*	page;
	page_t*	prev_page;
	ulint	space;
	ulint	prev_page_no;

	page = buf_frame_align(rec);
	if(page_rec_get_next(page_get_infimum_rec(page) != rec))
		return;

	/*获得rec前一页的page对象*/
	space = buf_frame_get_space_id(page);
	prev_page_no = btr_page_get_prev(page, mtr);

	ut_ad(prev_page_no != FIL_NULL);
	prev_page = buf_page_get_with_no_latch(space, prev_page_no, mtr);

	/*确定已经拥有x-latch*/
	ut_ad(mtr_memo_contains(mtr, buf_block_align(prev_page), MTR_MEMO_PAGE_X_FIX));
	/*前一个page的supremum继承rec上的锁*/
	lock_rec_reset_and_inherit_gap_locks(page_get_supremum_rec(prev_page), rec);
}

/*将update中的修改列数据更新到tuple逻辑记录当中*/
static void btr_cur_copy_new_col_vals(dtuple_t* entry, upd_t* update, mem_heap_t* heap)
{
	upd_field_t*	upd_field;
	dfield_t*	dfield;
	dfield_t*	new_val;
	ulint		field_no;
	byte*		data;
	ulint		i;

	dtuple_set_info_bits(entry, update->info_bits);

	/*将update中所有更改的列数据依次替换到dtuple对应的列中*/
	for(i = 0; i < upd_get_n_fields(update); i ++){
		upd_field = upd_get_nth_field(update, i);
		field_no = upd_field->field_no;
		dfield = dtuple_get_nth_field(entry, field_no);

		new_val = &(upd_field->new_val);
		if(new_val->len = UNIV_SQL_NULL)
			data = NULL;
		else{
			data = mem_heap_alloc(heap, new_val->len);
			ut_memcpy(data, new_val->data, new_val->len);
		}

		/*此处为0拷贝*/
		dfield_set_data(dfield, data, new_val->len);
	}
}

/*悲观式更新记录*/
ulint btr_cur_pessimistic_update(ulint flags, btr_cur_t* cursor, big_rec_t** big_rec, upd_t* update, ulint cmpl_info, que_thr_t* thr, mtr_t* mtr)
{
	big_rec_t*	big_rec_vec	= NULL;
	big_rec_t*	dummy_big_rec;
	dict_index_t*	index;
	page_t*		page;
	dict_tree_t*	tree;
	rec_t*		rec;
	page_cur_t*	page_cursor;
	dtuple_t*	new_entry;
	mem_heap_t*	heap;
	ulint		err;
	ulint		optim_err;
	ibool		dummy_reorganized;
	dulint		roll_ptr;
	trx_t*		trx;
	ibool		was_first;
	ibool		success;
	ulint		n_extents	= 0;
	ulint*		ext_vect;
	ulint		n_ext_vect;
	ulint		reserve_flag;

	*big_rec = NULL;

	page = btr_cur_get_page(cursor);
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;
	tree = index->tree;

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*先尝试乐观式的更新*/
	optim_err = btr_cur_optimistic_update(flags, cursor, update, cmpl_info, thr, mtr);
	if(optim_err != DB_UNDERFLOW && optim_err != DB_OVERFLOW)
		return optim_err;

	err = btr_cur_upd_lock_and_undo(flags, cursor, update, cmpl_info, thr, &roll_ptr);
	if(err != DB_SUCCESS)
		return err;

	if(optim_err == DB_OVERFLOW){
		n_extents = cursor->tree_height / 16 + 3;
		if(flags & BTR_NO_UNDO_LOG_FLAG)
			reserve_flag = FSP_CLEANING;
		else
			reserve_flag = FSP_NORMAL;

		/*尝试扩大表空间*/
		success = fsp_reserve_free_extents(cursor->index->space, n_extents, reserve_flag, mtr);
		if(!success)
			err = DB_OUT_OF_FILE_SPACE;
		return err;
	}
	
	heap = mem_heap_create(1024);
	trx = thr_get_trx(thr);
	new_entry = row_rec_to_index_entry(ROW_COPY_DATA, index, rec, heap);

	/*将更新列数据同步到new_entry中*/
	btr_cur_copy_new_col_vals(new_entry, update, heap);

	/*记录锁转到infimum上临时保存*/
	lock_rec_store_on_page_infimum(rec);

	btr_search_update_hash_on_delete(cursor);

	if(flags & BTR_NO_UNDO_LOG_FLAG){
		ut_a(big_rec_vec == NULL);
		btr_rec_free_updated_extern_fields(index, rec, update, TRUE, mtr);
	}

	ext_vect = mem_heap_alloc(heap, Size(ulint) * rec_get_n_fields(rec));
	n_ext_vect = btr_push_update_extern_fields(ext_vect, rec, update);

	/*删除游标处的记录*/
	page_cur_delete_rec(page_cursor, mtr);
	page_cur_move_to_prev(page_cursor);

	/*是个大记录,进行big_rec转换*/
	if((rec_get_converted_size(new_entry) >= page_get_free_space_of_empty() / 2)
		|| (rec_get_converted_size(new_entry) >= REC_MAX_DATA_SIZE)) {
			big_rec_vec = dtuple_convert_big_rec(index, new_entry, ext_vect, n_ext_vect);
			if(big_rec_vec == NULL){
				mem_heap_free(heap);
				goto return_after_reservations;
			}
	}

	/*将tuple插入到page中*/
	rec = btr_cur_insert_if_possible(cursor, new_entry, &dummy_reorganized, mtr);
	if(rec != NULL){
		/*插入成功，将寄存在infimum中的行锁转移回来*/
		lock_rec_restore_from_page_infimum(rec, page);
		rec_set_field_extern_bits(rec, ext_vect, n_ext_vect, mtr);
		
		if(!rec_get_deleted_flag(rec))
			btr_cur_unmark_extern_fields(rec, mtr);

		/*TODO:??*/
		btr_cur_compress_if_useful(cursor, mtr);

		err = DB_SUCCESS;
		mem_heap_free(heap);

		goto return_after_reservations;
	}

	/*判断游标是不是指向最开始的记录infimum*/
	if(page_cur_is_before_first(page_cursor))
		was_first = TRUE;
	else
		was_first = FALSE;

	/*尝试用乐观式插入tuple*/
	err = btr_cur_pessimistic_insert(BTR_NO_UNDO_LOG_FLAG
		| BTR_NO_LOCKING_FLAG
		| BTR_KEEP_SYS_FLAG, cursor, new_entry, &rec, &dummy_big_rec, NULL, mtr);
	ut_a(rec);
	ut_a(err == DB_SUCCESS);
	ut_a(dummy_big_rec == NULL);

	rec_set_field_extern_bits(rec, ext_vect, n_ext_vect, mtr);

	if(!rec_get_deleted_flag(rec))
		btr_cur_unmark_extern_fields(rec, mtr);

	lock_rec_restore_from_page_infimum(rec, page);

	/*如果没有infimum，那么行锁可能全部转移到了前一页的supremum上,尤其是在分裂的时候可能产生这个*/
	if(!was_first)
		btr_cur_pess_upd_restore_supremum(rec, mtr);

	mem_heap_free(heap);

return_after_reservations:
	if(n_extents > 0)
		fil_space_release_free_extents(cursor->index->space, n_extents);

	*big_rec = big_rec_vec;

	return err;
}


