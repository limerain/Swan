/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include <linux/device-mapper.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/dm-io.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>

#define DM_MSG_PREFIX "striped"
#define DM_IO_ERROR_THRESHOLD 15
#define minor_shift 4
#define num_flag_per_page (4096/sizeof(struct flag_nodes))
#define gc_buffer_size 50
#define GC_Weight 4
#define Targeted_Weight 3
#define Writing_Weight 2
#define Writed_Weight 1
#define Clean_Weight 0
#define KIZIL_POLICY 0
#define WEATHERING_RATIO 30

struct frc{
	char* buf;
	unsigned long long msector;
};

struct reverse_nodes{
	sector_t index;
	unsigned char dirty;
};

struct flag_nodes{
	sector_t msector;
	unsigned int wp;
	unsigned int num_moved;//////this is may cause performance degrade... it will move in reverse node
};

struct flag_set{
	struct flag_nodes** table;
	struct kmem_cache* node_buf;
	struct reverse_nodes** reverse_table;
};

struct buf_set{
	char *buf;
	unsigned long long index;
	unsigned long long sector;
};

struct gc_set{
	unsigned char set_num;
	struct task_struct *r_id;
	struct task_struct *w_id;
	struct buf_set *bs;
	struct dm_target *ti;
	struct mutex *gc_lock;

	unsigned int tp;
	unsigned int gp;
	sector_t tp_io_sector;
	unsigned int ptr_ovflw_size;
	char *kijil_map;
	unsigned long long tp_table_size;
	unsigned long long kijil_size;
	char phase_flag;
	unsigned char tp_clean_flag;
};

struct vm {
	struct dm_dev *dev;
	sector_t physical_start;
	sector_t end_sector;
	unsigned int main_dev;
	unsigned int maj_dev;
	unsigned long long num_dirty;
	unsigned char gen;

	atomic_t error_count;
};

struct vm_c {
	uint32_t vms;
	int vms_shift;

	/* The size of this target / num. stripes */
	sector_t vm_width;

	uint32_t chunk_size;
	int chunk_size_shift;

	/* Needed for handling events */
	struct dm_target *ti;

	/* Work struct used for triggering events*/
	struct work_struct trigger_event;
	/* volume manager variable*/
	unsigned int wp;//device Write pointer
	unsigned char *gp_list; //need do gc device
	unsigned long long *ws;//in device Write sector pointer
	unsigned long long *d_num;
	unsigned long long num_entry;// number of table's entry
	unsigned char mig_flag;
	unsigned int num_map_block;
	unsigned int num_gp;
	unsigned char overload;
	struct flag_set* fs;
	struct gc_set* gs;
	struct mutex lock;
	struct mutex gc_lock;
	unsigned long long read_index;
	unsigned long long cur_sector;
	unsigned char gc_flag;
	struct dm_io_client *io_client;
	unsigned int debug;

	struct vm vm[0];
};

static int read_job(struct gc_set *);
static int write_job(struct gc_set *);
/*static struct flag_nodes* vm_lfs_map_sector(struct vm_c *vc, sector_t target_sector,
		unsigned int wp, sector_t *write_sector, struct block_device **bdev, unsigned long bi_rw);*/
/*
 * An event is triggered whenever a drive
 * drops out of a stripe volume.
 */
static int atoj(const char *name){//ascii to major number
	int val;
	for(val=0;;name++){
		val = 10 *val + (*name - '0');
		if(*name == ':'){
			break;
		}
	}
	return val;
}

static int atom(const char *name){//ascii to minor number
	int val;
	for(;;name++){
		if(*name == ':'){
			name++;
			break;
		}
	}
	for(val=0;;name++){
		switch(*name){
			case '0'...'9':
				val = 10 *val + (*name - '0');
				break;
			default:
				return val;
		}
	}

	return val;
}

static void trigger_event(struct work_struct *work)
{
	struct vm_c *vc = container_of(work, struct vm_c,
					   trigger_event);
	printk("trigger event\n");
	dm_table_event(vc->ti->table);
}

static inline struct vm_c *alloc_context(unsigned int vms)
{
	size_t len;

	if (dm_array_too_big(sizeof(struct vm_c), sizeof(struct vm),
			     vms))
		return NULL;

	len = sizeof(struct vm_c) + (sizeof(struct vm) * vms);

	//return kmalloc(len, GFP_NOFS);
	return kmalloc(len, GFP_KERNEL);
}

/*
 * Parse a single <dev> <sector> pair
 */
static int get_vm(struct dm_target *ti, struct vm_c *vc,
		      unsigned int vm, char **argv)
{
	unsigned long long start;
	char dummy;

	if (sscanf(argv[1], "%llu%c", &start, &dummy) != 1)
		return -EINVAL;

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			  &vc->vm[vm].dev))
		return -ENXIO;

	vc->vm[vm].physical_start = start;

	return 0;
}

/*
 * Construct a striped mapping.
 * <number of stripes> <chunk size> [<dev_path> <offset>]+
 */
static int vm_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct vm_c *vc;
	sector_t width, tmp_len;
	uint32_t vms;
	uint32_t chunk_size;
	int r;
	unsigned long long i;

	if (argc < 2) {
		ti->error = "Not enough arguments";
		return -EINVAL;
	}

	if (kstrtouint(argv[0], 10, &vms) || !vms) {
		ti->error = "Invalid stripe count";
		return -EINVAL;
	}

	if (kstrtouint(argv[1], 10, &chunk_size) || !chunk_size) {
		ti->error = "Invalid chunk_size";
		return -EINVAL;
	}

	width = ti->len;
	if (sector_div(width, vms)) {
		ti->error = "Target length not divisible by "
		    "number of stripes";
		return -EINVAL;
	}

	tmp_len = width;
	if (sector_div(tmp_len, chunk_size)) {
		ti->error = "Target length not divisible by "
		    "chunk size";
		return -EINVAL;
	}

	/*
	 * Do we have enough arguments for that many stripes ?
	 */
	if (argc != (2 + 2 * vms)) {
		ti->error = "Not enough destinations "
			"specified";
		return -EINVAL;
	}

	vc = alloc_context(vms);
	if (!vc) {
		ti->error = "Memory allocation for striped context "
		    "failed";
		return -ENOMEM;
	}

	INIT_WORK(&vc->trigger_event, trigger_event);

	/* Set pointer to dm target; used in trigger_event */
	vc->ti = ti;
	vc->vms = vms;
	vc->vm_width = width;

	if (vms & (vms - 1))
		vc->vms_shift = -1;
	else
		vc->vms_shift = __ffs(vms);

	r = dm_set_target_max_io_len(ti, chunk_size);
	if (r) {
		kfree(vc);
		return r;
	}

	ti->num_flush_bios = vms;
	ti->num_discard_bios = vms;
	ti->num_write_same_bios = vms;

	vc->chunk_size = chunk_size;
	if (chunk_size & (chunk_size - 1))
		vc->chunk_size_shift = -1;
	else
		vc->chunk_size_shift = __ffs(chunk_size);

	/*
	 * Get the stripe destinations.
	 */
	for (i = 0; i < vms; i++) {
		argv += 2;

		r = get_vm(ti, vc, i, argv);
		if (r < 0) {
			ti->error = "Couldn't parse stripe destination";
			while (i--)
				dm_put_device(ti, vc->vm[i].dev);
			kfree(vc);
			return r;
		}
		atomic_set(&(vc->vm[i].error_count), 0);
	}

	/*volume manager initialize*/
	vc->wp = 0;//////current 0 is NVMe
	//vc->wp = 1;
	vc->ws = kmalloc(sizeof(unsigned long long) * vc->vms, GFP_KERNEL);
	for(i = 0; i<vc->vms; i++)
		vc->ws[i] = 0;
	vc->gp_list = kmalloc(sizeof(char) * vc->vms, GFP_KERNEL);
	vc->num_gp = 0;
	vc->io_client = dm_io_client_create();
	vc->gs = NULL;
	vc->overload = 0;

	for(i=0; i<vc->vms; i++)
		vc->gp_list[i] = Clean_Weight;//0 is clean
	vc->gp_list[vc->wp] = Writing_Weight;// first write ptr is already selected.

	{
		unsigned long long tem, disk_size;
		
		tem = 0;
		for(i = 0; i<vms; i++){
			struct block_device *cur_bdev = vc->vm[i].dev->bdev;
			vc->vm[i].end_sector = i_size_read(cur_bdev->bd_inode)>>9;//unit of sector
			printk("vm%llu start_sector %llu, end_sector %llu, target_offset %llu\n",
					i, (unsigned long long) vc->vm[i].physical_start, (unsigned long long) vc->vm[i].end_sector, (unsigned long long)dm_target_offset(ti, vc->ws[i]));
			disk_size = vc->vm[i].end_sector * 512;
			do_div(disk_size, (unsigned long long) vc->vm[i].dev->bdev->bd_block_size);
			tem += disk_size;
		}
		vc->num_entry = tem;//num entry is blk num
	}
	printk("num entry is %llu, node size is %lu, req mem is %llu\n", vc->num_entry, sizeof(struct flag_nodes), sizeof(struct flag_nodes) * vc->num_entry);
	
	//flag set initialize
	vc->fs = (struct flag_set *) kmalloc(sizeof(struct flag_set), GFP_KERNEL);
	vc->fs->node_buf = kmem_cache_create("dirty_data_buf", sizeof(struct flag_nodes),
			0, (SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD), NULL);

	vc->fs->table = (struct flag_nodes **)vmalloc(sizeof(struct flag_nodes*) * vc->num_entry);
	for(i=0; i<vc->num_entry; i++){
		//vc->fs->table[i] = NULL;//late alloc code
		vc->fs->table[i] = kmem_cache_alloc(vc->fs->node_buf, GFP_KERNEL);//pre alloc start
		vc->fs->table[i]->msector = -1;
		vc->fs->table[i]->wp = -1;//pre alloc end
		vc->fs->table[i]->num_moved = 0;
	}
	vc->num_map_block = 0;//vc->num_entry * sizeof(struct flag_nodes) / 4096;
	//vc->ws[0] += vc->num_map_block;

	vc->fs->reverse_table = vmalloc(sizeof(struct reverse_nodes*) * vc->vms);
	vc->d_num = kmalloc(sizeof(unsigned long long) * vc->vms, GFP_KERNEL);
	for(i=0; i<vc->vms; i++){
		unsigned long long j;
		unsigned long long r_table_size = (vc->vm[i].end_sector + 7);
		unsigned long long phy_sect = vc->vm[i].physical_start;
		do_div(phy_sect, 8);
		do_div(r_table_size, 8);
		printk("r_table_size = %llu\n", r_table_size);
		vc->vm[i].num_dirty = r_table_size - phy_sect;
		vc->d_num[i] = vc->vm[i].num_dirty;
		vc->fs->reverse_table[i] = vmalloc(sizeof(struct reverse_nodes) * r_table_size);
		for(j=0; j<r_table_size; j++){
			vc->fs->reverse_table[i][j].index = -1;
			vc->fs->reverse_table[i][j].dirty = 1;///1 is clean
		}
		//printk("%u's first ptr is %p, final ptr is %p\n", i, &(vc->fs->reverse_table[i][0]), &(vc->fs->reverse_table[i][j]));

	}

	for(i=0; i<vc->vms; i++){
		unsigned int minor = atom(vc->vm[i].dev->name);
		unsigned int major = atoj(vc->vm[i].dev->name);

		printk("dev name is %s\t", vc->vm[i].dev->name);
		if(major != 2600) vc->vm[i].main_dev = minor >> minor_shift;
		else vc->vm[i].main_dev = minor - 1;
		vc->vm[i].maj_dev = major;
		printk("main %u, maj %u\n", vc->vm[i].main_dev, vc->vm[i].maj_dev);
	}

	vc->mig_flag = 0;
	mutex_init(&vc->lock);
	mutex_init(&vc->gc_lock);

	if(KIZIL_POLICY){
		unsigned int i, j, r_size, l_size, gen;
		r_size = vc->vms; l_size = vc->vms;
		j = 0; gen = 0;
		while(l_size){
			if(l_size/2>0)
				r_size = l_size/2;
			else
				r_size = 1;
			//l_size/2>0 ? r_size = l_size/2 : r_size = 1;
			//printk("l_size %u, r_size %u, gen %u, j %u\n", l_size, r_size, gen, j);
			for(i=0; i<r_size; i++){
				vc->vm[j].gen = gen;
				j++;
			}
			gen++;
			l_size-= r_size;
		}
		
		for(i=0; i<vc->vms; i++)
			printk("%u's gen is %u\t", i, vc->vm[i].gen);
		printk("\n");
	}
	else vc->vm[i].gen = 0;

	ti->private = vc;
	vc->gs = kmalloc(sizeof(struct gc_set) * gc_buffer_size, GFP_KERNEL);
	for(i=0; i<gc_buffer_size; i++){
		vc->gs[i].set_num = i;
		vc->gs[i].ti = ti;
		vc->gs[i].gc_lock = &vc->gc_lock;
		vc->gs[i].kijil_map = NULL;
		vc->gs[i].bs = kmalloc(sizeof(struct buf_set), GFP_KERNEL);
		vc->gs[i].bs->buf = vmalloc(4096*127);
		vc->gs[i].r_id = kthread_run((void*)read_job, &vc->gs[i], "read_th");
		vc->gs[i].w_id = kthread_run((void*)write_job, &vc->gs[i], "write_th");
		vc->gs[i].phase_flag = -1;
		vc->gs[i].tp_clean_flag = 0;
	}
	vc->gc_flag = 0;

	ti->discards_supported = true;
	
	return 0;
}

static void vm_dtr(struct dm_target *ti)
{
	unsigned int i;
	struct vm_c *vc = (struct vm_c *) ti->private;

	for (i = 0; i < vc->vms; i++)
		dm_put_device(ti, vc->vm[i].dev);

	flush_work(&vc->trigger_event);

	/*for(i=0;i<vc->vms;i++){
		temp = vc->fs->wp[i];
		if(temp == NULL) continue;
		do{
			struct flag_node* del_node = temp;
			temp = temp->next;
			flag_erase(vc->fs, del_node);
		}while(temp->next != NULL);
		vc->fs->wp[i] = NULL;
	}*/
	/*if(vc->th_id[0]){
		kthread_stop(vc->th_id[0]);
		vc->th_id[0] = NULL;
	}
	if(vc->th_id[1]){
		kthread_stop(vc->th_id[1]);
		vc->th_id[1] = NULL;
	}*/
	/*if(vc->th_id){
		kthread_stop(vc->th_id);
		vc->th_id = NULL;
	}*/
	/*if(vc->th_id){
		kthread_stop(vc->th_id);
		vc->th_id = NULL;
	}
	if(vc->gs){
		kfree(vc->gs);
		vc->gs = NULL;
	}*/
	vfree(vc->fs->table);

	kfree(vc);
}

inline int do_kijil(struct vm_c* vc, int gp){
	unsigned long long disk_block_size = vc->vm[gp].end_sector+7;//initialize for do_div
	signed char num_count = 0;
	unsigned long long i = 0;
	char* kijil_map = vmalloc(disk_block_size);
	struct reverse_nodes* gp_reverse_table = vc->fs->reverse_table[gp];
	unsigned int prev_moved = 0;
	int kijil_size = 0;

	do_div(disk_block_size, 8);
	///why kijil grain is 1 byte?? can more coars grain??

	//printk("rv table print start\n");
	//for(i=0; i<disk_block_size; i++){
	//	printk("%llu:%u ", i, gp_reverse_table[i].dirty);
	//	if(i !=0 && i%30 == 0)
	//		printk("\n");
	//}
	//printk("rv table print end\n");

	if(gp_reverse_table[i].dirty == 0) num_count = 1;
	else if(gp_reverse_table[i].dirty >= 1)	num_count = -1;
	prev_moved = vc->fs->table[gp_reverse_table[i].index]->num_moved;
	for(i=1; i<disk_block_size; i++){///already check 0 index, modified to (i=0)
		unsigned int cur_moved = vc->fs->table[gp_reverse_table[i].index]->num_moved;
		if(num_count > 0){
			if(num_count == 127){//range over
				kijil_map[kijil_size] = num_count;
				kijil_size++;
				num_count = 0;
			}
			if(KIZIL_POLICY && prev_moved == cur_moved && gp_reverse_table[i].dirty == 0) num_count++;
			else if(gp_reverse_table[i].dirty == 0) num_count++; //continuous valid blk
			else{//valid is end
				kijil_map[kijil_size] = num_count;
				kijil_size++;
				if(KIZIL_POLICY && gp_reverse_table[i+1].dirty == 0) num_count = 1;
				else num_count = -1;
			}
		}
		else if(num_count < 0){
			if(num_count == -127){//range over
				kijil_map[kijil_size] = num_count;//recording count
				kijil_size++;
				num_count = 0;
			}
			if(KIZIL_POLICY && prev_moved == cur_moved && gp_reverse_table[i].dirty >= 1) num_count--;
			else if(gp_reverse_table[i].dirty >= 1) num_count--;//continuous invalid blk
			else{//invalid is end
				kijil_map[kijil_size] = num_count;
				kijil_size++;
				if(KIZIL_POLICY && gp_reverse_table[i+1].dirty >= 1) num_count = -1;
				else num_count = 1;
			}
		}
		else printk("unknown else error\n");
		prev_moved = cur_moved;
	}
	kijil_map[kijil_size++] = num_count;
	//end doing kijil

	//for(i=0; i<kijil_size; i++){//Printing kijil_map
	//	if(kijil_map[i] <0)
	//		printk("%llu:%d ", i, kijil_map[i]);
	//	else
	//		printk("%llu:+%d ", i, kijil_map[i]);
	//	if(i != 0 && i%30 == 0)
	//		printk("\n");
	//}
	//printk("kijil_map print end\n");

	vc->gs[0].kijil_map = vmalloc(kijil_size);
	memcpy(vc->gs[0].kijil_map, kijil_map, kijil_size);
	for(i=1; i<gc_buffer_size; i++)
		vc->gs[i].kijil_map = vc->gs[0].kijil_map;
	vfree(kijil_map); kijil_map = NULL;

	return kijil_size;
}

inline char point_targeting(struct vm_c *vc, int *r_tp, int *r_gp){//r_tp, r_gp is return_tp, return gp
	unsigned int tp, i, wp_main_dev, min, min_weight;
	unsigned int wp_maj_dev;
	unsigned long long percent_of_dirtied;

	if(*r_gp == -1){
		for(i=0; i<vc->vms; i++){
			if(vc->gp_list[i] == GC_Weight){
				*r_gp = i;
				break;
			}
			else if(i == vc->vms-1 && vc->gp_list[i] != GC_Weight){
				printk("no existence gc ssd\n");
				vc->mig_flag = 0;
				return 0;
			}
		}
	}
	tp = *r_gp;
	wp_main_dev = vc->vm[vc->wp].main_dev;
	wp_maj_dev = vc->vm[vc->wp].maj_dev;
	min = tp; min_weight = -1;

	vc->num_gp = 0;////num_gp is counting now.
	for(i=0; i<vc->vms; i++){
		unsigned weight = 0;

		percent_of_dirtied = (vc->vm[i].num_dirty - vc->d_num[i]) * 100;
		do_div(percent_of_dirtied, vc->vm[i].num_dirty);
		if(!KIZIL_POLICY) printk("%u's(%u) valid ratio is %llu\t", i, vc->gp_list[i], percent_of_dirtied);

		if(vc->gp_list[i] == GC_Weight && percent_of_dirtied != 0)
			vc->num_gp++;
		tp = (tp + 1) % vc->vms;
		if(vc->vm[tp].maj_dev == wp_maj_dev && vc->vm[tp].main_dev == wp_main_dev)
			weight = 5;
		weight += vc->gp_list[tp];

		if(weight == 1){
			unsigned long long percent_of_ptr_location;///////thisthisthis
			percent_of_ptr_location = (vc->ws[tp] + vc->vm[tp].physical_start) * 100;///calculate remained sectors
			do_div(percent_of_ptr_location, vc->vm[tp].end_sector);
			if(percent_of_dirtied + percent_of_ptr_location < 100){
				min = tp;
				break;
			}
		}

		if(min_weight > weight){//search target device by minimal weight
			min = tp;
			min_weight = weight;
			wp_maj_dev = vc->vm[min].maj_dev;
			wp_main_dev = vc->vm[min].main_dev;
		}
		weight = 0;
	}
	for(;i<vc->vms; i++){
		percent_of_dirtied = (vc->vm[i].num_dirty - vc->d_num[i]) * 100;
		do_div(percent_of_dirtied, vc->vm[i].num_dirty);
		//printk("%u's weight is %u, gen %u, valid ratio is %llu\t", i, vc->gp_list[i], vc->vm[i].gen, percent_of_dirtied);
		if(!KIZIL_POLICY) printk("%u's (%u) valid ratio is %llu\t", i, vc->gp_list[i], percent_of_dirtied);
	}
	if(!KIZIL_POLICY){
		printk("\n");
		if(vc->gp_list[min] == Writed_Weight) vc->gp_list[min]++;///then weight is 2.(Writing pointer)
		else vc->gp_list[min] = Targeted_Weight;///target pointer is 3
	}
	//printk("in ptr targeting, gp_list ++ %u\n", vc->gp_list[min]);
	*r_tp = min;
	
	return 1;
}

inline char weathering_check(struct vm_c *vc){
	if(vc->num_gp >= 1){
		unsigned int i;
		unsigned int tp = 0, gp = -1;
		unsigned long long kijil_size = 1;
		unsigned long long percent_of_dirtied = 0;
		unsigned int min = 0;
		unsigned int min_percent = 100;

		////this function has a bug that change weight of writed SSD to 0, unexpectledly
		if(point_targeting(vc, &tp, &gp) == 0){
			//printk("ptr targeting failed\n");
			if(vc->gp_list[tp] == Targeted_Weight) vc->gp_list[tp] -= Targeted_Weight;///target pointer is 3
			else if(vc->gp_list[tp] == Writing_Weight) vc->gp_list[tp]--;//then weight return to Writed_Weight
			return 0;
		}

		for(i=0; i<vc->vms; i++){
			if(vc->gp_list[i] != GC_Weight) continue;
			percent_of_dirtied = (vc->vm[i].num_dirty - vc->d_num[i]) * 100;
			do_div(percent_of_dirtied, vc->vm[i].num_dirty);
			if(percent_of_dirtied < min_percent){
				min = i; min_percent = percent_of_dirtied;
		}}

		if(min_percent > WEATHERING_RATIO){//sufficiently weatheringed.
			if(vc->vms - vc->num_gp <= 2){
				//printk("vms is %u, num_gp is %u\n", vc->vms, vc->num_gp);
				if(vc->gp_list[tp] == Targeted_Weight) vc->gp_list[tp] -= Targeted_Weight;///target pointer is 3
				else if(vc->gp_list[tp] == Writing_Weight) vc->gp_list[tp]--;
				
				if(vc->gp_list[min] != GC_Weight){
					//printk("no exist GP\n");
					return 0;
				}
				gp = min;
				if(point_targeting(vc, &tp, &gp) == 0){
					//printk("ptr targeting\n");
					if(vc->gp_list[tp] == Targeted_Weight) vc->gp_list[tp] -= Targeted_Weight;///target pointer is 3
					else if(vc->gp_list[tp] == Writing_Weight) vc->gp_list[tp]--;
					return 0;
				}
			}
			else{
				printk("not yet\n");
				if(vc->gp_list[tp] == Targeted_Weight) vc->gp_list[tp] -= Targeted_Weight;///target pointer is 3
				else if(vc->gp_list[tp] == Writing_Weight) vc->gp_list[tp]--;
				return 0;
			}
		}

		percent_of_dirtied = (vc->vm[gp].num_dirty - vc->d_num[gp]) * 100;
		do_div(percent_of_dirtied, vc->vm[gp].num_dirty);
		printk("dirty_ratio is %llu\n", percent_of_dirtied);

		printk("mig is start\n");

		//printk("kijil\n");
		kijil_size = do_kijil(vc, gp);////kijil_mapping

		for(i=0; i<gc_buffer_size; i++){
			vc->gs[i].ptr_ovflw_size = 0;
			vc->gs[i].tp_io_sector = 0;
			vc->gs[i].tp_table_size = 0;
			vc->gs[i].tp = tp;
			vc->gs[i].gp = gp;
			vc->gs[i].kijil_size = kijil_size;
			vc->gs[i].phase_flag = -1;
		}

		vc->read_index = 0;
		vc->cur_sector = 0;
		printk("gp_count %u, gp %u, tp %u, kijil_size %llu\n", vc->num_gp, gp, tp, kijil_size);
		return 1;
	}
	return 0;
}

inline void map_store(struct vm_c *vc){
	struct dm_io_region io;
	struct dm_io_request io_req;
	sector_t map_ptr = 0;
	char* buf_for_store = vmalloc(4096);
	struct flag_nodes *table = vmalloc(sizeof(struct flag_nodes) * num_flag_per_page);
	table[0].msector = 52;
	table[0].wp = 0;
	table[1].msector = 33;
	table[1].wp = 0;
	table[2].msector = 21;
	table[2].wp = 1;

	memcpy(buf_for_store, (char*) table, 4096);

	io_req.bi_rw = WRITE; io_req.mem.type = DM_IO_VMA;
	io_req.mem.ptr.vma = buf_for_store;
	io_req.client = vc->io_client;
	io_req.notify.fn = NULL;

	io.bdev = vc->vm[0].dev->bdev;
	io.sector = vc->vm[0].physical_start + map_ptr * 8;
	io.count = 8;

	printk("store start\n");
	dm_io(&io_req, 1, &io, NULL);
	printk("store end\n");

	vfree(buf_for_store);
	buf_for_store = vmalloc(4096);

	io_req.bi_rw = READ; io_req.mem.type = DM_IO_VMA;
	io_req.mem.ptr.vma = buf_for_store;
	io_req.client = vc->io_client;
	io_req.notify.fn = NULL;

	io.bdev = vc->vm[0].dev->bdev;
	io.sector = vc->vm[0].physical_start + map_ptr * 8;
	io.count = 8;

	printk("load start\n");
	dm_io(&io_req, 1, &io, NULL);
	printk("load end\n");
	printk("0 sector %llu, wp %u, 1 sector %llu, wp %u, 2 sector %llu, wp %u\n",
			(unsigned long long) table[0].msector, table[0].wp, (unsigned long long) table[1].msector, table[1].wp, (unsigned long long) table[2].msector, table[2].wp);
}

static int write_job(struct gc_set* gs){
	struct dm_target *ti = gs->ti;
	struct vm_c *vc = ti->private;
	unsigned long long write_index, cur_sector;
	struct dm_io_region io;
	struct dm_io_request io_req;
	unsigned int i, size;
	struct reverse_nodes* tp_reverse_table = NULL;
	struct reverse_nodes* gp_reverse_table = NULL;

	io_req.bi_rw = WRITE; io_req.mem.type = DM_IO_VMA;
	io_req.mem.ptr.vma = gs->bs->buf; io_req.notify.fn = NULL;
	io_req.client = vc->io_client;

	while(1){
		if(vc->mig_flag == 1){
			if(gs->kijil_map != NULL){//outer gc is now started.
				gp_reverse_table = vc->fs->reverse_table[gs->gp];
				write_index = 0;
				cur_sector = 0;
				while(1){//...this condition is ... may have problem... 
					if(gs->phase_flag == 1){//write is able.
						struct buf_set *c_bs = gs->bs;
						unsigned long long g_tis;
						//unsigned int tp = gs->tp;
						tp_reverse_table = vc->fs->reverse_table[gs->tp];

						size = gs->kijil_map[c_bs->index];
						cur_sector = c_bs->sector;
						gs->tp_table_size = vc->vm[gs->tp].end_sector + 7;
						do_div(cur_sector, 8);

						//printk("%u's write setting\n", gs->set_num);

						mutex_lock(&vc->lock);{//modified reverse_table information
							gs->tp_io_sector = vc->ws[gs->tp] + vc->vm[gs->tp].physical_start;
							g_tis = vc->ws[gs->tp];
							do_div(g_tis, 8);

							//printk("%u's before for loop\n", gs->set_num);
							for(i=0; i<size; i++){
								//unsigned int next_tp = (gs->tp+1) % vc->vms;
								if(vc->ws[gs->tp] + vc->vm[gs->tp].physical_start + 8 > vc->vm[gs->tp].end_sector){
									unsigned int next_point, gp_main_dev, gp_maj_dev, min, min_weight, weight;
									
									//printk("%u's in over range\n", gs->set_num);
									vc->gp_list[gs->tp] = GC_Weight;
									vc->num_gp++;
									
									next_point = gs->tp;
									gp_main_dev = vc->vm[gs->tp].main_dev;
									gp_maj_dev = vc->vm[gs->tp].maj_dev;
									min = next_point; min_weight = -1;
									weight = 0;
									
									for(i = 0; i < vc->vms; i++){
										next_point = (next_point + 1) % vc->vms;
										if(vc->vm[next_point].maj_dev == gp_maj_dev && vc->vm[next_point].main_dev == gp_main_dev)
											weight = 5;
										weight+= vc->gp_list[next_point];

										if(min_weight > weight){
											min = next_point;
											min_weight = weight;
											gp_maj_dev = vc->vm[min].maj_dev;
											gp_main_dev = vc->vm[min].main_dev;
										}
										weight = 0;
									}
									if(min_weight != 0) vc->overload = 1;
									
									//printk("%u's after find next pointer\n", gs->set_num);
									vc->gp_list[min] = Targeted_Weight;
									//printk("over flow!!! next tp is %d, %s\n", min, vc->vm[min].dev->name);
									gs->tp = min;
									if(vc->mig_flag == 0) vc->mig_flag = 1;
									//printk("%u's setting over range\n", gs->set_num);
									
									if(KIZIL_POLICY){
										unsigned long long cs = cur_sector;
										unsigned int cur_moved = 0;
										unsigned int writied_tp = -1;
										unsigned int i;
										unsigned flag = 0;
										gs->tp = -1;
										
										do_div(cs, 8);
										cur_moved = vc->fs->table[vc->fs->reverse_table[gs->gp][cs].index]->num_moved;
										for(i=0; i<vc->vms; i++){
											unsigned int min_weight = 0;
											if(vc->gp_list[i] >= Writing_Weight) continue;
											//then weight is Clean or Writied
											if(vc->vm[i].gen == cur_moved){
												if(vc->gp_list[i] == Clean_Weight){
													//printk("%u's find fitable clean SSD is %u\n", gs->set_num, i);
													gs->tp_clean_flag = 1;
													gs->tp = i;
													flag = 1;
													break;
												}
												else{
													if(min_weight > vc->gp_list[i]){
														min_weight = vc->gp_list[i];
														writied_tp = i;
														flag = 2;
													}}
											}
										}
										//printk("%u's writied_tp %u, vc->vms %u\n", gs->set_num, writied_tp, vc->vms);
										if(flag == 1)
											vc->gp_list[gs->tp] += Writed_Weight;
										else if(flag == 2){
											gs->tp = writied_tp;
										}
										else{
											//printk("%u's pt targeting...\n", gs->set_num);
											if(point_targeting(vc, &gs->tp, &gs->gp) == 0){
												//printk("%u's pt targeting error\n", gs->set_num);
											}
											//printk("%u's after pt targeting tp %u\n", gs->set_num, gs->tp);
										}
										//printk("so,(in read) %u's tp is %u, ....???? gp is %u\n", gs->set_num, gs->tp, gs->gp);
										//if(gs->tp == -1) printk("%u's reading fucking targeting error\n", gs->set_num);
									}
								}
								//printk("%u's before tp setting\n", gs->set_num);
								tp_reverse_table[g_tis + i].index = gp_reverse_table[cur_sector + i].index;
								tp_reverse_table[g_tis + i].dirty = gp_reverse_table[cur_sector + i].dirty;

								//printk("%u's after tp setting\n", gs->set_num);
								//if(gs->set_num == 0) printk("0's index %llu's write sector %llu, index %llu\n", c_bs->index, (cur_sector + i) * 8, gp_reverse_table[cur_sector + i].index);
								vc->ws[gs->tp] += 8;
								if(tp_reverse_table[g_tis+i].dirty != 1) vc->d_num[gs->tp]--;///why not 0??
							}
						}mutex_unlock(&vc->lock);

						//printk("%u's io setting\n", gs->set_num);
						io.bdev = vc->vm[gs->tp].dev->bdev;///need to modify
						//if overflow occur, then tp is need to change
						io.sector = gs->tp_io_sector;
						io.count = (gs->kijil_map[c_bs->index] - gs->ptr_ovflw_size) * 8;

						//printk("%d's write index %llu, sector %llu, real sector %llu, size %llu\n", gs->set_num, c_bs->index, c_bs->sector, (unsigned long long)io.sector, (unsigned long long)io.count);
						dm_io(&io_req, 1, &io, NULL);
						//sync io is finished.

						//printk("%u's io end\n", gs->set_num);
						size-= gs->ptr_ovflw_size;

						if(size != 0){
							for(i=0; i<size; i++){
								struct reverse_nodes *rn;
								if(g_tis + i > gs->tp_table_size) break;
								rn = &(tp_reverse_table[g_tis + i]);
								if(rn->index == -1) continue;//index -1 is a non writed sector
								mutex_lock(&vc->lock);//is this overhead??
								vc->fs->table[rn->index]->msector = gs->tp_io_sector + (i * 8);
								vc->fs->table[rn->index]->wp = gs->tp;
								vc->fs->table[rn->index]->num_moved++;
								mutex_unlock(&vc->lock);
							}
							//printk("%u's table setting is end\n", gs->set_num);
						}
						if(gs->ptr_ovflw_size != 0){//need to verify...
							unsigned int j, next_tp;
							printk("oGC's write overflow occur\n");
							gs->kijil_map[c_bs->index] = size;
							gs->ptr_ovflw_size = 0;
							mutex_lock(&vc->lock);
							next_tp = (gs->tp + 1) % vc->vms;//need to apply point targeting algorithms
							for(j=0; j<gc_buffer_size; j++)
								vc->gs[j].tp = next_tp;
							mutex_unlock(&vc->lock);
						}
						///judge to next operation
						if(!(vc->gc_flag & 2)){///all read job is not end.
							if(KIZIL_POLICY){
								if(gs->tp_clean_flag == 1){
									gs->tp_clean_flag = 0;
									vc->gp_list[gs->tp] -= Writed_Weight;
								}
							}
							gs->phase_flag = 0;//this operations means ready to read job
						}
						else{//read job is end..
							//printk("0. %d's write job is end\n", gs->set_num);
							if(gs->phase_flag != -2)
								gs->phase_flag = -2;
							break;//my write job is end
						}
						///////if phase_flag == -2, then all thread's read job is end.
						//////therefore my write job is endest write job.
						//////if phase_flag != -2, then read job is not end. therefore continue for Outer GC's read job
					}
					else if(gs->phase_flag == -2) break;
					else{//holding!!
						msleep(1);
					}
				}
				///this code section is escape loop(write is finished).
				//therefore can trimming SSD
				{//this section is waiting for all write job is end.
					char wait_flag = 1;
					if(gs->set_num == 0){
						//printk("0's wait start\n");
						while(wait_flag){//if wait_flag == 1, infinite loop
							msleep(5);
							for(i=0; i<gc_buffer_size; i++){
								if(vc->gs[i].phase_flag != -2){
									//printk("not end set is %d\n", i);
									wait_flag = 2;// flag is at least one of not ended-SSD is exist.
								}
							}

							if(wait_flag == 2) wait_flag = 1;
							else wait_flag = 0;
						}
						vc->gc_flag |= 4;
					}
				}
				//if(gs->set_num == 0){
				//	printk("discard start\n");
				//	for(i=0; i<gc_buffer_size; i++){
				//		if(vc->gs[i].phase_flag != -2){
				//			printk("not end set is %d\n", i);
				//		}
				//	}
				//}
				//and... we discard all data in GC SSD
				if(vc->gc_flag & 4 && gs->set_num == 0){//TRIM command perform only 0 GC set.
					//io_req.bi_rw = REQ_WRITE | REQ_DISCARD;
					//io_req.mem.ptr.vma = gs->bs->buf;
					//io.bdev = vc->vm[gs->gp].dev->bdev;
					//io.sector = vc->vm[gs->gp].physical_start;
					//io.count = vc->vm[gs->gp].end_sector - 1 - vc->vm[gs->gp].physical_start;

					//dm_io(&io_req, 1, &io, NULL);////discard by DM_IO
					blkdev_issue_discard(vc->vm[gs->gp].dev->bdev, vc->vm[gs->gp].physical_start,
							vc->vm[gs->gp].end_sector - 1 - vc->vm[gs->gp].physical_start, GFP_NOFS, 0);
					//discard is finished.
					printk("dirty_num is %llu\n", vc->d_num[gs->gp]);
					vc->d_num[gs->gp] = vc->vm[gs->gp].num_dirty;
					printk("end discard\n");

					if(gs->tp != gs->gp)
						vc->ws[gs->gp] = 0;
					if(gs->gp == 0) vc->ws[0]+= vc->num_map_block;//current num_map_block is 0. because for debugging
					if(!KIZIL_POLICY) vc->gp_list[gs->tp] = Writed_Weight;//targeted pointer is 1
					vc->gp_list[gs->gp] = Clean_Weight;//0 means clean
					if(!KIZIL_POLICY) printk("tp is %u, gp is %u\n", gs->tp, gs->gp);
					if(!KIZIL_POLICY) printk("tp's ws is %llu\n", vc->ws[gs->tp]);

					vfree(vc->gs[0].kijil_map);///other kijil_map is replica
					for(i=gc_buffer_size; i>0; i--){
						vc->gs[i].kijil_map = NULL;
						vc->gs[i].phase_flag = 0;
					}
					vc->gc_flag = 0;

					//vc->overhead = 0;
					vc->mig_flag = 0;
					vc->num_gp--;
					for(i=0; i<vc->vms; i++){
						//printk("%u's weight is %u\n", i, vc->gp_list[i]);
						if(vc->gp_list[i] == GC_Weight){
							printk("detect gp! is ... %u\n", i);
							vc->mig_flag = 1;
							//vc->overload = 1;
							break;
						}
					}

				}
				else{//wait TRIM job is end for other GC set. 
					//printk("%d's write TRIM wait\n", gs->set_num);
					ssleep(1);
				}
			}
			else{//if kijil_map is NULL,
				//wait for weathering before kijil_mapping
				//printk("%d's write kijil_map NULL wait...\n", gs->set_num);
				ssleep(1);
			}
		}
		else {//mig flag is 0, 0 is wait for filling SSD
			//printk("%d's write job is wait...\n", gs->set_num);
			ssleep(1);
		}
	}
	return 0;
}

static int read_job(struct gc_set *gs){
	struct dm_target *ti = gs->ti;
	struct vm_c *vc = ti->private;
	struct dm_io_region io;
	struct dm_io_request io_req;
	unsigned long long read_index = 0;
	unsigned long long cur_sector = 0;

	io_req.bi_rw = READ; io_req.mem.type = DM_IO_VMA;
	io_req.mem.ptr.vma = gs->bs->buf;
	io_req.notify.fn = NULL; io_req.client = vc->io_client;

	while(1){
		if(vc->mig_flag == 1){
			if(gs->kijil_map != NULL && vc->gc_flag & 1){
				if(gs->phase_flag == -1){
					gs->phase_flag = 0;//phase_flag initialize, flag 0 is read phase
					read_index = 0;
					cur_sector = 0;
				}
				while(1){
					if(gs->phase_flag == 0){
						mutex_lock(gs->gc_lock);
						read_index = vc->read_index;//avoid to problem
						cur_sector = vc->cur_sector;
						if(read_index >= gs->kijil_size) gs->phase_flag = -2;
						while(gs->kijil_map[read_index] <= 0){//if invalid
							//printk("%d's skip index %llu, sector %llu, size %llu\n", gs->set_num, read_index, cur_sector, (unsigned long long)(-gs->kijil_map[read_index]) * 8);
							cur_sector-= gs->kijil_map[read_index] * 8; //kijil_map[index] value is negative. therefore cur_sector+= minus(negative value)
							read_index++;//index & sector ptr is increase
							if(read_index >= gs->kijil_size){
								//this code section is case of last index is invalid.
								//therefore rad job is required to end
								gs->phase_flag = -2;
								break;
							}
						}
						if(gs->phase_flag == -2 || read_index >= gs->kijil_size){
							gs->phase_flag = -2;
							mutex_unlock(gs->gc_lock);
							vc->gc_flag|= 2;
							break;///read lable break.
						}
						vc->read_index = read_index + 1;//index and,
						vc->cur_sector = cur_sector + gs->kijil_map[read_index] * 8;//cur_sector value is up to date.
						mutex_unlock(gs->gc_lock);

						gs->bs->index = read_index;
						gs->bs->sector = cur_sector;
						if(KIZIL_POLICY){
							unsigned long long cs = cur_sector;
							unsigned int cur_moved = 0;
							unsigned int writied_tp = -1;
							unsigned int i;
							unsigned flag = 0;
							gs->tp = -1;
							
							do_div(cs, 8);
							cur_moved = vc->fs->table[vc->fs->reverse_table[gs->gp][cs].index]->num_moved;
							for(i=0; i<vc->vms; i++){
								unsigned int min_weight = 0;
								if(vc->gp_list[i] >= Writing_Weight) continue;
								//then weight is Clean or Writied
								if(vc->vm[i].gen == cur_moved){
									if(vc->gp_list[i] == Clean_Weight){
										//printk("%u's find fitable clean SSD is %u\n", gs->set_num, i);
										gs->tp_clean_flag = 1;
										gs->tp = i;
										flag = 1;
										break;
									}
									else{
										if(min_weight > vc->gp_list[i]){
											min_weight = vc->gp_list[i];
											writied_tp = i;
											flag = 2;
									}}
								}
							}
							//printk("%u's writied_tp %u, vc->vms %u\n", gs->set_num, writied_tp, vc->vms);
							//if(vc->gp_list[gs->tp] == Clean_Weight)
							if(flag == 1){
								gs->tp_clean_flag = 1;
								vc->gp_list[gs->tp] += Writed_Weight;
							}
							//else if(vc->gp_list[writied_tp] == Writed_Weight){
							else if(flag == 2){
								//printk("%u's find fitable writied SSD\n", gs->set_num);
								gs->tp = writied_tp;
							}
							else{
								//printk("%u's pt targeting...\n", gs->set_num);
								if(point_targeting(vc, &(gs->tp), &(gs->gp)) == 0){
									//printk("%u's pt targeting error\n", gs->set_num);
								}
								printk("%u's after pt targeting tp %u\n", gs->set_num, gs->tp);
							}
							//printk("so,(in read) %u's tp is %u, ....???? gp is %u\n", gs->set_num, gs->tp, gs->gp);
							if(gs->tp == -1) printk("%u's reading fucking targeting error\n", gs->set_num);
						}

						/*if(gs->set_num == 0){//may be it is debug code
							unsigned long long tcur_sector = cur_sector - vc->vm[gs->gp].physical_start; do_div(tcur_sector, 8);
							unsigned int size = gs->kijil_map[read_index];
							for(i = 0; i<size; i++)	printk("0's index %llu's read sector %llu, index %llu\n", read_index, (tcur_sector + i) * 8, vc->fs->reverse_table[gs->gp][tcur_sector + i].index);
						}*/
						//setting for read DM_IO
						io.bdev = vc->vm[gs->gp].dev->bdev;
						io.sector = cur_sector;
						io.count = gs->kijil_map[read_index] * 8;
						//setting is end
						//printk("%d's read index %llu, sector %llu, size %llu\n", gs->set_num, read_index, cur_sector, (unsigned long long)io.count);
						dm_io(&io_req, 1, &io, NULL);

						gs->phase_flag = 1;
					}
					else{//gs->phase_flag != 1, then holding
						if(gs->phase_flag == -2)
							break;
						//printk("%d's holding.. pf = %d, gf %d\n", gs->set_num, gs->phase_flag, vc->gc_flag);
						msleep(1);//sleeping...
					}
				}
				{
					//need to wait for other gc_set's operation
					//printk("%d's gs op wait\n", gs->set_num);
					ssleep(1);
				}
			}
			else{//if kijil_map is NULL,
				//wait for weathering before kijil_mapping
				//weathering check perform only 1 gc_set.
				//printk("%d's weathering wait\n", gs->set_num);
				if(gs->set_num == 0 && weathering_check(vc) == 1){//return value 1 is success
					vc->gc_flag |= 1;
					continue;
				}
			}
			ssleep(1);
		}
		else{//mig flag is 0, 0 is wait for filling SSD
			//printk("%d's read job is wait...\n", gs->set_num);
			if(gs->set_num == 0){
				unsigned long long percent_of_dirtied;
				unsigned int i = 0;
				
				for(i=0; i<vc->vms; i++){
					percent_of_dirtied = (vc->vm[i].num_dirty - vc->d_num[i]) * 100;
					do_div(percent_of_dirtied, vc->vm[i].num_dirty);
					printk("%u's(%u) valid ratio is %llu\t", i, vc->gp_list[i], percent_of_dirtied);
				}
				printk("\n");
			}
			ssleep(1);
		}
	}
	return 0;
}

static void vm_map_range_sector(struct vm_c *vc, sector_t sector,
				    uint32_t target_vm, sector_t *result)
{
	uint32_t vm = 0;
	sector_t cur_sector;

	for(cur_sector = vc->vm[vm].physical_start +  sector;
			cur_sector > vc->vm[vm].end_sector;
			cur_sector -= vc->vm[vm].end_sector, vm++);

	if (vm == target_vm)
		return;
}

static int vm_map_range(struct vm_c *vc, struct bio *bio,
			    uint32_t target_vm)
{
	sector_t begin=0, end=0;
	vm_map_range_sector(vc, bio->bi_iter.bi_sector,
			target_vm, &begin);
	vm_map_range_sector(vc, bio_end_sector(bio),/////end sector is start sect + size
			target_vm, &end);

	if (begin < end) {
		bio->bi_bdev = vc->vm[target_vm].dev->bdev;
		bio->bi_iter.bi_sector = begin +
			vc->vm[target_vm].physical_start;
		bio->bi_iter.bi_size = to_bytes(end - begin);
		return DM_MAPIO_REMAPPED;
	} else {
		// The range doesn't map to the target stripe 
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}
}

static void read_callback(unsigned long error, void* context){
	char* buf = (char*) context;
	printk("read contents : \n");
	printk("%s\n", buf);
	/*struct frc* temp = (struct frc*) context;
	printk("%llu's read contents : \n", temp->msector);
	printk("%s\n", temp->buf);*/
}

static inline struct flag_nodes* vm_lfs_map_sector(struct vm_c *vc, struct bio* bio){
	struct flag_set *fs = vc->fs;
	unsigned long long index = bio->bi_iter.bi_sector;
	unsigned int remainder = 0;
	unsigned long bi_rw = bio_rw(bio);
	remainder = do_div(index, 8);

	//printk("access! io is %lu, size %u, wp %u, index %llu, sector %llu\n", bi_rw, bio_sectors(bio), fs->table[index]->wp, index, bio->bi_iter.bi_sector);
	if(bi_rw == WRITE){
		unsigned long long dirtied_sector = fs->table[index]->msector;
		unsigned int i;
		unsigned int sectors = bio_sectors(bio);
		unsigned long long phy_sector;
		unsigned long long cur_ws, cur_index;

		/*char* addr;////it is print contents of page in BIO code(for debugging)

		addr = phys_to_virt(page_to_pfn(bio->bi_io_vec->bv_page)<<PAGE_SHIFT);
		printk("write buffer(len is %u) contents is : \n", bio->bi_io_vec->bv_len);
		printk("'%s'\n", addr);*/
		/*printk("'%s'\t", addr);
		for(i=0; i<bio->bi_io_vec->bv_len; i++){
			printk("%x ", *(addr + i));
		}
		printk("\n");*/
		
		mutex_lock(&vc->lock);
		
		if(dirtied_sector != -1){
			unsigned int dirtied_wp = 0;
			unsigned long long dindex = index;
			i=0;
			
			while(i<sectors){
				dirtied_wp = fs->table[dindex]->wp;
				dirtied_sector = fs->table[dindex]->msector;

				if(dirtied_sector != -1){
					do_div(dirtied_sector, 8);
					if(fs->reverse_table[dirtied_wp][dirtied_sector].dirty == 1){
						i+=8; dindex++;
						continue;
					}
					fs->reverse_table[dirtied_wp][dirtied_sector].dirty = 2;///2 is invalid
					vc->d_num[dirtied_wp]++;
				}
				i+= 8; dindex++;
			}
		}
		if(vc->ws[vc->wp] + vc->vm[vc->wp].physical_start + sectors > vc->vm[vc->wp].end_sector){
			unsigned int next_point, gp_main_dev, gp_maj_dev, min, min_weight, weight, second, second_weight;

			printk("ws %llu, start %llu, bi_sector %llu, end_sector %llu, sectors %u\n", (unsigned long long)vc->ws[vc->wp], (unsigned long long)vc->vm[vc->wp].physical_start, (unsigned long long)bio->bi_iter.bi_sector, (unsigned long long)vc->vm[vc->wp].end_sector, sectors);

			vc->gp_list[vc->wp] = GC_Weight;///GC ptr is 3
			vc->num_gp++;

			next_point = vc->wp;
			gp_main_dev = vc->vm[vc->wp].main_dev;
			gp_maj_dev = vc->vm[vc->wp].maj_dev;
			min = next_point; min_weight = -1;
			second = -1; second_weight = -1;///second is only write ptr policy.
			weight = 0;

			printk("in mapping\t");
			for(i = 0; i < vc->vms; i++){
				next_point = (next_point + 1) % vc->vms;
				if(vc->vm[next_point].maj_dev == gp_maj_dev && vc->vm[next_point].main_dev == gp_main_dev)
					weight = 5;

				weight+= vc->gp_list[next_point];
				printk("%u's weight is %u\t", next_point, weight);

				if(KIZIL_POLICY){
					if(vc->gp_list[next_point] >= 2) weight+= vc->vms - 2;////
					else{//0 or 1
						weight+= vc->vm[next_point].gen * 2;
					}
				}
				if(min_weight > weight){
					second = min; second_weight = min_weight;
					min = next_point;
					min_weight = weight;
					gp_maj_dev = vc->vm[min].maj_dev;
					gp_main_dev = vc->vm[min].main_dev;
				}
				weight = 0;
			}
			printk("\n");
			if(min_weight != 0) vc->overload = 1;

			printk("big!! next wp is %d, %s\n", min, vc->vm[min].dev->name);
			vc->wp = min;
			vc->gp_list[vc->wp] = Writing_Weight;//write ptr
			if(vc->mig_flag == 0) vc->mig_flag = 1;
		}

		fs->table[index]->msector = vc->ws[vc->wp];
		vc->ws[vc->wp]+= sectors;
		fs->table[index]->wp = vc->wp;
		mutex_unlock(&vc->lock);

		fs->table[index]->msector+= vc->vm[vc->wp].physical_start;

		i = 0; phy_sector = fs->table[index]->msector;
		cur_ws = fs->table[index]->msector;	cur_index = index;
		do_div(phy_sector, 8);
		//printk("i %u, sectors %u, wp %u, index %llu, msector %llu, psector %llu\n", i, sectors, vc->wp, index, fs->table[index]->msector, phy_sector);

		while(i < sectors){////this is fully record in map table
			fs->table[cur_index]->wp = fs->table[index]->wp;
			fs->table[cur_index]->msector = cur_ws;
			fs->table[cur_index]->num_moved = 0;

			fs->reverse_table[vc->wp][phy_sector].index = cur_index;
			fs->reverse_table[vc->wp][phy_sector].dirty = 0;
			
			i+= 8; phy_sector++; cur_index++; cur_ws+= 8; vc->d_num[vc->wp]--;
		}

		bio->bi_bdev = vc->vm[vc->wp].dev->bdev;
		bio->bi_iter.bi_sector = fs->table[index]->msector + remainder;
	}
	else{//read
		if(fs->table[index]->msector == -1){//first access
			sector_t return_sector;

			return_sector = vc->ws[vc->wp];
			return_sector+= vc->vm[vc->wp].physical_start;

			bio->bi_bdev = vc->vm[vc->wp].dev->bdev;
			bio->bi_iter.bi_sector = return_sector + remainder;
		}
		else{
			//char *buf = (char *) vmalloc(bio->bi_io_vec->bv_len);
			/*char *buf = (char*) vmalloc(4096);///////it is print some contents of page in SSD code
			struct dm_io_region io;
			struct dm_io_request io_req;
			
			//struct frc temp;
			//temp.buf = (char*) vmalloc(4096);
			//temp.msector = fs->table[index]->msector;
			//struct page_list *pages;
			
			io_req.bi_rw = READ; io_req.mem.type = DM_IO_VMA;
			io_req.mem.ptr.vma = buf;
			io_req.notify.fn = read_callback; io_req.client = vc->io_client;
			io_req.notify.context = buf;
			
			io.bdev = vc->vm[fs->table[index]->wp].dev->bdev;
			io.sector = fs->table[index]->msector;
			io.count = bio->bi_io_vec->bv_len/8;
			io.count = 8;

			dm_io(&io_req, 1, &io, NULL);*/

			//printk("read buf contents is : \n");
			//printk("%s\n", buf);

			/*io_req.bi_rw = READ; io_req.mem.type = DM_IO_PAGE_LIST;
			io_req.mem.ptr.pl = pages;
			io_req.notify.fn = NULL; io_req.client = vc->io_client;

			io.bdev = vc->vm[fs->table[index]->wp].dev->bdev;
			io.sector = fs->table[index]->msector + remainder;
			io.count = bio->bi_io_vec->bv_len;

			dm_io(&io_req, 1, &io, NULL);*/

			//printk("wp %u, bv_len %u, bv_offset %u, sector %llu, msector %llu\n", fs->table[index]->wp, bio->bi_io_vec->bv_len, bio->bi_io_vec->bv_offset, bio->bi_iter.bi_sector, fs->table[index]->msector+remainder);
			bio->bi_bdev = vc->vm[fs->table[index]->wp].dev->bdev;
			bio->bi_iter.bi_sector = fs->table[index]->msector + remainder;
		}
	}
	return fs->table[index];
}

static inline void vm_lfs_map_bio(struct dm_target *ti, struct bio *bio){
	struct vm_c *vc = ti->private;
	struct flag_nodes* temp;
	
	if(bio_sectors(bio))
		temp = vm_lfs_map_sector(vc, bio);
}

static int vm_map(struct dm_target *ti, struct bio *bio){
	struct vm_c *vc =  ti->private;
	unsigned target_bio_nr;

	if(bio->bi_rw & REQ_FLUSH){
		printk("flush\n");
		target_bio_nr = dm_bio_get_target_bio_nr(bio);
		BUG_ON(target_bio_nr >= vc->vms);
		bio->bi_bdev = vc->vm[target_bio_nr].dev->bdev;
		return DM_MAPIO_REMAPPED;
	}
	if(unlikely(bio->bi_rw & REQ_DISCARD)){
		unsigned long long index = bio->bi_iter.bi_sector;
		unsigned int remainder;
		//unsigned long bi_rw = bio_rw(bio);
		unsigned int i=0;
		unsigned int sectors = bio_sectors(bio);
		unsigned long long dirtied_sector;
		unsigned int dirtied_wp;

		//printk("TRIM! io is %lu, size %u, wp %u, remainder %u, index %llu, sector %llu\n", bi_rw, bio_sectors(bio), vc->fs->table[index]->wp, remainder, index, bio->bi_iter.bi_sector);
		remainder = do_div(index, 8);

		while(i<sectors){
			dirtied_sector = vc->fs->table[index]->msector;
			dirtied_wp = vc->fs->table[index]->wp;

			if(dirtied_sector != -1){
				do_div(dirtied_sector, 8);
				if(vc->fs->reverse_table[dirtied_wp][dirtied_sector].dirty == 1){
					i+= 8; index++;
					continue;
				}
				vc->fs->reverse_table[dirtied_wp][dirtied_sector].dirty = 2;///2 is invalid;
				vc->d_num[dirtied_wp]++;

				vc->fs->table[index]->msector = -1;	vc->fs->table[index]->wp = -1;
			}
			i+= 8; index++;
		}

		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}
	else if(unlikely(bio->bi_rw & REQ_WRITE_SAME)){
		printk("write same\n");
		target_bio_nr = dm_bio_get_target_bio_nr(bio);
		BUG_ON(target_bio_nr >= vc->vms);
		return vm_map_range(vc, bio, target_bio_nr);
	}
	else {
		//printk("bi_rw %lu\n", bio->bi_rw);
	}

	vm_lfs_map_bio(ti, bio);

	return DM_MAPIO_REMAPPED;
}

/*
 * Stripe status:
 *
 * INFO
 * #stripes [stripe_name <stripe_name>] [group word count]
 * [error count 'A|D' <error count 'A|D'>]
 *
 * TABLE
 * #stripes [stripe chunk size]
 * [stripe_name physical_start <stripe_name physical_start>]
 *
 */

static void vm_status(struct dm_target *ti, status_type_t type,
			  unsigned status_flags, char *result, unsigned maxlen)
{
	struct vm_c *vc = (struct vm_c *) ti->private;
	char buffer[vc->vms + 1];
	unsigned int sz = 0;
	unsigned int i;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%d ", vc->vms);
		for (i = 0; i < vc->vms; i++)  {
			DMEMIT("%s ", vc->vm[i].dev->name);
			buffer[i] = atomic_read(&(vc->vm[i].error_count)) ?
				'D' : 'A';
		}
		buffer[i] = '\0';
		DMEMIT("1 %s", buffer);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%d %llu", vc->vms,
			(unsigned long long)vc->chunk_size);
		for (i = 0; i < vc->vms; i++)
			DMEMIT(" %s %llu", vc->vm[i].dev->name,
			    (unsigned long long)vc->vm[i].physical_start);
		break;
	}
}

static int vm_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	unsigned i;
	char major_minor[16];
	struct vm_c *vc = ti->private;

	if (!error)
		return 0; /* I/O complete */

	if ((error == -EWOULDBLOCK) && (bio->bi_rw & REQ_RAHEAD))
		return error;

	if (error == -EOPNOTSUPP)
		return error;

	memset(major_minor, 0, sizeof(major_minor));
	sprintf(major_minor, "%d:%d",
		MAJOR(disk_devt(bio->bi_bdev->bd_disk)),
		MINOR(disk_devt(bio->bi_bdev->bd_disk)));

	/*
	 * Test to see which stripe drive triggered the event
	 * and increment error count for all stripes on that device.
	 * If the error count for a given device exceeds the threshold
	 * value we will no longer trigger any further events.
	 */
	for (i = 0; i < vc->vms; i++)
		if (!strcmp(vc->vm[i].dev->name, major_minor)) {
			atomic_inc(&(vc->vm[i].error_count));
			if (atomic_read(&(vc->vm[i].error_count)) <
			    DM_IO_ERROR_THRESHOLD)
				schedule_work(&vc->trigger_event);
		}

	return error;
}

static struct target_type vm_target = {
	.name   = "striped",
	.version = {1, 5, 1},
	.module = THIS_MODULE,
	.ctr    = vm_ctr,
	.dtr    = vm_dtr,
	.map    = vm_map,
	.end_io = vm_end_io,
	.status = vm_status,
};

int __init dm_stripe_init(void)
{
	int r;

	r = dm_register_target(&vm_target);
	if (r < 0) {
		DMWARN("target registration failed");
		return r;
	}

	return r;
}

void dm_stripe_exit(void)
{
	dm_unregister_target(&vm_target);
}

