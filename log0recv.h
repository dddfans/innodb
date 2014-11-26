#ifndef __log0recv_h
#define __log0recv_h

#include "univ.h"
#include "ut0byte.h"
#include "page0types.h"
#include "hash0hash.h"
#include "log0log.h"

/*********************************************api************************************/
/*���checkpoint��Ϣ*/
ibool				recv_read_cp_info_for_backup(byte* hdr, dulint* lsn, ulint* offset, ulint fsp_limit, dulint* cp_no, dulint* cp_no, dulint* first_header_lsn);
/*ɨ��һ��logƬ�Σ�������Чblock��n_byte_scanned���Ⱥ�scanned_checkpoint_no*/
void				recv_scan_log_seg_for_backup(byte* buf, ulint buf_len, dulint* scanned_lsn, ulint* scanned_checkpoint_no, ulint n_byte_scanned);

UNIV_INLINE ibool	recv_recovery_is_on();

UNIV_INLINE ibool	recv_recovery_from_backup_is_on();

void				recv_recover_page(ibool recover_backup, ibool just_read_in, page_t* page, ulint space, ulint page_no);

ulint				recv_recovery_from_checkpoint_start(ulint type, dulint limit_lsn, dulint min_flushed_lsn, dulint max_flushed_lsn);

void				recv_recovery_from_checkpoint_finish();

ibool				recv_scan_log_recs(ibool apply_automatically, ulint available_memory, ibool store_to_hash, byte* buf, 
							ulint len, dulint start_lsn, dulint* contiguous_lsn, dulint* group_scanned_lsn);

void				recv_reset_logs(dulint lsn, ulint arch_log_no, ibool new_logs_created);

void				recv_reset_log_file_for_backup(char* log_dir, ulint n_log_files, ulint log_file_size, dulint lsn);

void				recv_sys_create();

void				recv_sys_init(ibool recover_from_backup, ulint available_memory);

void				recv_apply_hashed_log_recs(ibool allow_ibuf);

void				recv_apply_log_recs_for_backup(ulint n_data_files, char** data_files, ulint* file_sizes);

ulint				recv_recovery_from_archive_start(dulint min_flushed_lsn, dulint limit_lsn, ulint first_log_no);

void				recv_recovery_from_archive_finish();

void				recv_compare_spaces();

void				recv_compare_spaces_low(ulint space1, ulint space2, ulint n_pages);

/********************************************************/
typedef struct recv_data_struct	recv_data_t;
struct recv_data_struct
{
	recv_data_t*	next;
};

typedef struct recv_struct recv_t;
struct recv_struct
{
	byte			type;
	recv_data_t*	data;
	dulint			start_lsn;
	dulint			end_lsn;
	UT_LIST_NODE_T(recv_t)	rec_list;
};

typedef struct recv_addr_struct recv_addr_t;
struct recv_addr_struct
{
	ulint			state;
	ulint			space;
	ulint			page_no;
	UT_LIST_BASE_NODE_T(recv_t) rec_list;
	hash_node_t		addr_hash;
};

typedef struct recv_sys_struct recv_sys_t;
struct recv_sys_struct
{
	mutex_t			mutex;
	ibool			apply_log_recs;
	ibool			apply_batch_on;
	
	dulint			lsn;
	ulint			last_log_buf_size;

	byte*			last_block;
	byte*			last_block_buf_start;
	byte*			buf;

	ulint			len;
	dulint			parse_start_len;
	dulint			scanned_lsn;

	ulint			scanned_checkpoint_no;
	ulint			recovered_offset;

	dulint			recovered_lsn;
	dulint			limit_lsn;

	ibool			found_corrupt_log;

	log_group_t*	archive_group;
	mem_heap_t*		heap;
	hash_table_t*	addr_hash;
	ulint			n_addrs;
};

extern recv_sys_t*		recv_sys;
extern ibool			recv_recovery_on;
extern ibool			recv_no_ibuf_operations;
extern ibool			recv_needed_recovery;

extern ibool			recv_is_making_a_backup;

/*2M*/
#define RECV_PARSING_BUF_SIZE	(2 * 1024 * 1204)

#define RECV_SCAN_SIZE			(4 * UNIV_PAGE_SIZE)

/*recv_addr_t->state type*/
#define RECV_NOT_PROCESSED		71
#define RECV_BEING_READ			72
#define RECV_BEING_PROCESSED	73
#define RECV_PROCESSED			74

#define RECV_REPLICA_SPACE_ADD	1

#define RECV_POOL_N_FREE_BLOCKS	(ut_min(256, buf_pool_get_curr_size() / 8))

#include "log0recv.inl"

#endif





