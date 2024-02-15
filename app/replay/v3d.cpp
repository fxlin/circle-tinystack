//
// myclass.cpp
//
#include <circle/bcm2835.h>
#include <circle/bcm2836.h>
#include <circle/bcm2711.h>
#include <circle/memio.h>
#include <circle/bcmpropertytags.h>
#include <circle/memory.h>
#include <circle/util.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>
//#include <linux/delay.h>	// this also includes sleep, bad.
#include <linux/atomic.h>
#include <linux/slab.h> // kmalloc
#include <linux/errno.h>

extern "C" {
	#include "../lib/zlib_inflate/zlib.h"
}

#include "elf.h"
#include "v3d.h"
#include "v3d_regs.h"
#include "v3d_replay_linux.h"

// from linux/delay.cpp
static void udelay (unsigned long usecs)
{
	CTimer::Get ()->usDelay (usecs);
}

static const char * FromKernel = "v3d";

static u32 *pt_paddr = NULL;
static void *mmu_scratch_paddr = NULL;

// cf: bcm2835_asb_enable
int asb_enable(u32 reg)
{
	if (!reg)
		return 0;

	/* Enable the module's async AXI bridges. */
	ASB_WRITE(reg, ASB_READ(reg) & ~ASB_REQ_STOP);
	CTimer::Get()->usDelay(1000);
	if (ASB_READ(reg) & ASB_ACK)
			return -1;
	else
		return 0;
}

int asb_disable(u32 reg)
{
	if (!reg)
		return 0;

	/* Enable the module's async AXI bridges. */
	ASB_WRITE(reg, ASB_READ(reg) | ASB_REQ_STOP);
	CTimer::Get()->usDelay(1000);
	if ((!ASB_READ(reg)) & ASB_ACK)
			return -1;
	else
		return 0;
}

void dump_v3d_regs(void)
{
	printk("V3D_HUB_IDENT0: %08x", V3D_READ(V3D_HUB_IDENT0));
	printk("V3D_HUB_IDENT1: %08x", V3D_READ(V3D_HUB_IDENT1));
	printk("V3D_HUB_IDENT2: %08x", V3D_READ(V3D_HUB_IDENT2));
	printk("V3D_HUB_IDENT3: %08x", V3D_READ(V3D_HUB_IDENT3));
	printk("V3D_MMU_DEBUG_INFO: %08x", V3D_READ(V3D_MMU_DEBUG_INFO));
}

void dump_asb_regs(void)
{
	printk("ASB_BRDG_VERSION: %08x (should be: 0)", ASB_READ(ASB_BRDG_VERSION));
	/* 0x62726467, "BRDG". cf bcm2835-power.c */
	printk("ASB_AXI_BRDG_ID: %08x (should be: 0x62726467)", ASB_READ(ASB_AXI_BRDG_ID));
}

/* cf: linux clk-raspberrypi.c */
enum rpi_firmware_clk_id {
	RPI_FIRMWARE_EMMC_CLK_ID = 1,
	RPI_FIRMWARE_UART_CLK_ID,
	RPI_FIRMWARE_ARM_CLK_ID,
	RPI_FIRMWARE_CORE_CLK_ID,
	RPI_FIRMWARE_V3D_CLK_ID,
	RPI_FIRMWARE_H264_CLK_ID,
	RPI_FIRMWARE_ISP_CLK_ID,
	RPI_FIRMWARE_SDRAM_CLK_ID,
	RPI_FIRMWARE_PIXEL_CLK_ID,
	RPI_FIRMWARE_PWM_CLK_ID,
	RPI_FIRMWARE_HEVC_CLK_ID,
	RPI_FIRMWARE_EMMC2_CLK_ID,
	RPI_FIRMWARE_M2MC_CLK_ID,
	RPI_FIRMWARE_PIXEL_BVB_CLK_ID,
	RPI_FIRMWARE_NUM_CLK_ID,
};

void dump_clk_states(void)
{
	CBcmPropertyTags Tags;
	TPropertyTagClockState st;
	int len = 256, cnt;
	char *buf = new char(len), *p = buf;
	assert(buf);

	cnt = snprintf(buf, len, "clock:state ");
	buf += cnt; len -= cnt;

	for (int i = 1; i < RPI_FIRMWARE_NUM_CLK_ID; i++) {
		st.nClockId = i;
		if (!Tags.GetTag (PROPTAG_GET_CLOCK_STATE, &st, sizeof st)) {
			printk("get tag failed");
			break;
		} else {
			cnt = snprintf(buf, len, "clk_%d:%u ", i, st.nState);
			buf += cnt; len -= cnt;
		}
	}

	printk("%s\n", p);
	delete p;
}

// set all clks on
void set_clk_states(void)
{
	// set all clock states
	CBcmPropertyTags Tags;
	TPropertyTagClockState st;
	st.nState = 1;

	for (int i = 1; i < RPI_FIRMWARE_NUM_CLK_ID; i++) {
		st.nClockId = i;
		if (!Tags.GetTag (PROPTAG_SET_CLOCK_STATE, &st, sizeof st))
			CLogger::Get()->Write (FromKernel, LogNotice, "set tag failed");
	}
}

/* These power domain indices are the firmware interface's indices
 * minus one.
 * linux: raspbeerrypi-power.h
 */
#define RPI_POWER_DOMAIN_I2C0		0
#define RPI_POWER_DOMAIN_I2C1		1
#define RPI_POWER_DOMAIN_I2C2		2
#define RPI_POWER_DOMAIN_VIDEO_SCALER	3
#define RPI_POWER_DOMAIN_VPU1		4
#define RPI_POWER_DOMAIN_HDMI		5
#define RPI_POWER_DOMAIN_USB		6
#define RPI_POWER_DOMAIN_VEC		7
#define RPI_POWER_DOMAIN_JPEG		8
#define RPI_POWER_DOMAIN_H264		9
#define RPI_POWER_DOMAIN_V3D		10
#define RPI_POWER_DOMAIN_ISP		11
#define RPI_POWER_DOMAIN_UNICAM0	12
#define RPI_POWER_DOMAIN_UNICAM1	13
#define RPI_POWER_DOMAIN_CCP2RX		14
#define RPI_POWER_DOMAIN_CSI2		15
#define RPI_POWER_DOMAIN_CPI		16
#define RPI_POWER_DOMAIN_DSI0		17
#define RPI_POWER_DOMAIN_DSI1		18
#define RPI_POWER_DOMAIN_TRANSPOSER	19
#define RPI_POWER_DOMAIN_CCP2TX		20
#define RPI_POWER_DOMAIN_CDP		21
#define RPI_POWER_DOMAIN_ARM		22

#define RPI_POWER_DOMAIN_COUNT		23

void dump_pd_states(void)
{
		CBcmPropertyTags Tags;
		TPropertyTagPowerState r;
		int len = 256, cnt;
		char *buf = new char(len), *p = buf;
		assert(buf);

		for (int i = 0; i < RPI_POWER_DOMAIN_COUNT; i++) {
			r.nDeviceId = i + 1;
			if (!Tags.GetTag (PROPTAG_GET_DOMAIN_STATE, &r, sizeof r))
				CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
			else {
				cnt = snprintf(buf, len, "%d:%u ", i, r.nState);
				buf += cnt; len -= cnt;
			}
		}

		printk("pd:state = %s\n", p);
		delete p;
}

// turn all power domains on.
void set_pd_states(void)
{
	CBcmPropertyTags Tags;
	TPropertyTagPowerState r;

	for (int i = 0; i < RPI_POWER_DOMAIN_COUNT; i++) {
		r.nDeviceId = i + 1;
		r.nState = 1;

		if (!Tags.GetTag (PROPTAG_SET_DOMAIN_STATE, &r, sizeof r))
			CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
	}
}

/* Note: All PTEs for the 1MB superpage must be filled with the
 * superpage bit set.
 */
#define V3D_PTE_SUPERPAGE BIT(31) // xzl: pte format: pfns shifted to lower bits
#define V3D_PTE_WRITEABLE BIT(29)
#define V3D_PTE_VALID BIT(28)

static int v3d_mmu_flush_all(void)
{
	int ret;
	unsigned t0 = CTimer::GetClockTicks();

	ret = wait_for(!(V3D_READ(V3D_MMU_CTL) &
				 V3D_MMU_CTL_TLB_CLEARING), 100);
	assert(!ret);

	V3D_WRITE(V3D_MMU_CTL, V3D_READ(V3D_MMU_CTL) |
		  V3D_MMU_CTL_TLB_CLEAR);

	V3D_WRITE(V3D_MMUC_CONTROL,
		  V3D_MMUC_CONTROL_FLUSH |
		  V3D_MMUC_CONTROL_ENABLE);

	ret = wait_for(!(V3D_READ(V3D_MMU_CTL) &
			 V3D_MMU_CTL_TLB_CLEARING), 100);

	assert(!ret);

	ret = wait_for(!(V3D_READ(V3D_MMUC_CONTROL) &
				 V3D_MMUC_CONTROL_FLUSHING), 100);

	assert(!ret);
	printk("%s ticks %u", __func__, CTimer::GetClockTicks() - t0);
	return ret;
}

// for a contig phys region,
// map @phys to gpu @virt
void v3d_mmu_insert_ptes(u32 phys, u32 page /*gpu virt*/, u32 npages)
{
	u32 page_prot = V3D_PTE_WRITEABLE | V3D_PTE_VALID;

	u32 page_address = phys >> V3D_MMU_PAGE_SHIFT;
	u32 pte = page_prot | page_address; // xzl: encoding phys addr
	u32 i;

	printk("xzl: %s: phys %08x", __func__, phys);

	assert(!(page_address + (PAGE_SIZE >> V3D_MMU_PAGE_SHIFT) >=
				 BIT(24)));

	// xzl: within one phys contig region, increment pfn (pte+i)
	for (i = 0; i < npages; i++)
		pt_paddr[page++] = pte + i;

	if (v3d_mmu_flush_all())
		printk("MMU flush timeout\n");
}

void v3d_mmu_remove_ptes(u32 virt, u32 npages)
{
	u32 page;

	for (page = (virt >> V3D_MMU_PAGE_SHIFT); page < page + npages; page++)
		pt_paddr[page] = 0;

	v3d_mmu_flush_all();
}

// --------------------- file io ------------------------ //
// Circle's own fatfs cannot seek
// #include <circle/fs/fat/fatfs.h>
// CFATFileSystem * the_fs = NULL;

#include <fatfs/ff.h>

FATFS the_fs;

/* wrapper around the fatfs */

#if 0 // around the circle's fatfs
// return 0 on failure.
static unsigned file_open(const char *path) {
	BUG_ON(!the_fs || !path);
	return the_fs->FileOpen(path);
}

// auto increments offset
// return 0: eof; non-zero: # of bytes read
static unsigned  file_read(unsigned file,
		void *data, unsigned int size)
{
	unsigned ret;
	BUG_ON(!the_fs);
	ret = the_fs->FileRead(file, data, size);
	BUG_ON(ret == 0xFFFFFFFF); // "general failure"

	return ret;
}

static void file_close(unsigned file) {
	unsigned ret;

	BUG_ON(!the_fs);

	ret = the_fs->FileClose(file);
	BUG_ON(!ret); // 0 on failure
}
#endif

//typedef FIL file;


// auto increments offset
// return 0: eof; non-zero: # of bytes read
//static unsigned  file_read(unsigned file,
//		void *data, unsigned int size)
//{
//	unsigned ret;
//	BUG_ON(!the_fs);
//	ret = the_fs->FileRead(file, data, size);
//	BUG_ON(ret == 0xFFFFFFFF); // "general failure"
//
//	return ret;
//}

#ifdef V3D_LOAD_FROM_FILE
// return 0 on ok
static int elf_read(FIL *fp, void *buf, u32 len, u32 pos)
{
	FRESULT res;
	u32 ll = 0;

	assert(fp);
	res = f_lseek(fp, pos);
	if (res != FR_OK)
		return -1;

	res = f_read(fp, buf, len, &ll);
	if (res != FR_OK || len != ll)
		return -1;
}
#endif

// return 0 on ok
static int elf_read_img(const char *elfbase, ssize_t elfsize,
		void *buf, u32 len, u32 pos)
{
	printk("buf %lx len %d pos %u", (u64)buf, len, pos);

	BUG_ON(!elfbase || !buf || pos + len > elfsize);
	memcpy(buf, elfbase + pos, len);
	return 0;
}

//static void file_close(FIL *fp) {
//	FRESULT res;
//
//	assert(fp);
//
//	res = f_close(fp);
//	assert(res == FR_OK);
//}

// --------------------- replay ------------------------ //
// bookkeeping all live GPU regions (BO)
struct gpu_region {
	u32 start_page; // gpu
	u32 num_pages; // gpu
	u32 num_bytes_dump;
//	struct drm_gem_shmem_object * shmem; // cpu kernel
	u32 phys; // cpu, identity mapping
	u32 type; // from bo type
	struct list_head region_list;
};

#define MAX_RECORDS 				65536		// failsafe
#define DEFAULT_DELAY_US		5000 	// per reg access.
#define WAIT_FOR_IRQ_TIMEOUT_US (5 * 1000 * 1000)

// xzl: from linux list.h
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

// For replay: BOs we've mapped during replay. will free at the end of replay
static LIST_HEAD(the_gpu_regions);
//static LIST_HEAD(the_shmem_freelist); // for caching shmem allocation

// ret 0 on ok
int add_gpu_region(u32 start_page, struct gpu_region *r)
{
	int ret = 0;
	struct list_head *c;
	struct gpu_region *region;

	assert(r);

	list_for_each(c, &the_gpu_regions) {
		region = list_entry(c, struct gpu_region, region_list);
		if (region->start_page == r->start_page) {
			ret = -1;
			goto out;
		}
	}
	list_add(&r->region_list, &the_gpu_regions);
out:
	return ret;
}

// ret the ptr on ok; NULL no found.
// caller shall free the gpu region struct??
struct gpu_region * _lookup_gpu_region(u32 start_page, bool remove)
{
	struct list_head *c, *tmp;
	struct gpu_region *region;

	list_for_each_safe(c, tmp, &the_gpu_regions) {
		region = list_entry(c, struct gpu_region, region_list);
		if (region->start_page == start_page) {
			if (remove)
				list_del(c); // unlink it
			return region;
		}
	}
	return NULL;
}

struct gpu_region *remove_gpu_region(u32 start_page)
{
	return _lookup_gpu_region(start_page, true);
}

struct gpu_region * lookup_gpu_region(u32 start_page)
{
	return _lookup_gpu_region(start_page, false);
}

//int v3d_replay_irqcnt = 0;
atomic_t v3d_replay_irqcnt = ATOMIC_INIT(0); // must be atomic

static void v3d_replay_cleanup(void) {
	// XXX free all gpu regions??
}

u64 ktime_get_ns()
{
	u32 sec, us; // no ns?

	CTimer::Get()->GetUniversalTime(&sec, &us);
	return sec * 1000 * 1000 * 1000 + us * 1000;
}

#ifdef V3D_LOAD_FROM_FILE
/* load all segs from an elf file. assuming the GPU mappings exist
 * @filesz: out, total file bytes loaded
 * cf: load_elf_library
 * return: error code. 0 okay
 * */
static int xzl_load_gpu_regions_elf(FIL *file, ssize_t *filesz)
{
	struct elf_phdr *elf_phdata;
	struct elf_phdr *eppnt;
	int retval, error, i, j;
	struct elfhdr elf_ex;

	*filesz = 0;

	error = -ENOEXEC;
	retval = elf_read(file, &elf_ex, sizeof(elf_ex), 0);
	if (retval != 0)
		goto out;

	if (memcmp(elf_ex.e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	/* First of all, some simple consistency checks */
	if (elf_ex.e_machine != ELF_ARCH) /* xzl: add more checks? */
		goto out;

	/* Now read in all of the header information */
	j = sizeof(struct elf_phdr) * elf_ex.e_phnum;
	/* j < ELF_MIN_ALIGN because elf_ex.e_phnum <= 2 */ //xzl-- matters?

	error = -ENOMEM;
	elf_phdata = (struct elf_phdr *)kmalloc(j, GFP_KERNEL);
	if (!elf_phdata)
		goto out;

	eppnt = elf_phdata;
	error = -ENOEXEC;
	retval = elf_read(file, eppnt, j, elf_ex.e_phoff);
	if (retval < 0)
		goto out_free_ph;

	// xzl: @eppnt: buf for all prog headers @j: sz of all program headers
	for (i = 0; i<elf_ex.e_phnum; i++) {
		u32 start_page;
		char *addr;
		struct gpu_region *rr;

		BUG_ON(eppnt[i].p_type != PT_LOAD);

		start_page = eppnt[i].p_vaddr >> V3D_MMU_PAGE_SHIFT;

		/* the GPU mem region must have been mapped */
#if 0
		for (j = 0; j < num_gpu_regions; j++) {
			if (the_gpu_regions[j].start_page == start_page) {
				shmem_obj = the_gpu_regions[j].shmem;
				break;
			}
		}
		if (j == num_gpu_regions) {
					DRM_ERROR("BUG? try to load a GPU region unmapped. start_page=%08x",
							start_page);
					goto out_free_ph;
				}
#endif

		rr = lookup_gpu_region(start_page);
			if (!rr) {
				printk("BUG? try to load a GPU region unmapped. start_page=%08x",
						start_page);
				goto out_free_ph;
			}
			addr = (char *)(long)(rr->phys); // phys is virt

		if ((rr->num_pages << V3D_MMU_PAGE_SHIFT) != eppnt[i].p_memsz) { /* sanity check */
			printk("sz %lx != memsz %llx filesz %llx",
					rr->num_pages << V3D_MMU_PAGE_SHIFT, eppnt[i].p_memsz,
					eppnt[i].p_filesz);
			goto out_free_ph;
		}

		/* map GPU BO to kernel space, upload mem contents */
		BUG_ON(!addr);
		retval = elf_read(file, addr, eppnt[i].p_filesz,
				eppnt[i].p_offset/*file offset*/);

		if (retval) {
			printk("elf_read failed. why?");
			continue;
		}

		*filesz += eppnt[i].p_filesz;

		printk("loadelf: vaddr %08llx memsz %08llx filesz %08llx",
				eppnt[i].p_vaddr, eppnt[i].p_memsz, eppnt[i].p_filesz);
	}

	error = 0;

out_free_ph:
	kfree(elf_phdata);
out:
	return error;
}
#endif // #ifdef V3D_LOAD_FROM_FILE

// from mem image
// elfsize: in

static int xzl_load_gpu_regions_elfimg(const char *elfbuf, ssize_t elfsize)
{
	struct elf_phdr *elf_phdata;
	struct elf_phdr *eppnt;
	int retval, error, i, j;
	struct elfhdr elf_ex;

	error = -ENOEXEC;
	retval = elf_read_img(elfbuf, elfsize, &elf_ex, sizeof(elf_ex), 0);
	if (retval != 0)
		goto out;

	if (memcmp(elf_ex.e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	/* First of all, some simple consistency checks */
	if (elf_ex.e_machine != ELF_ARCH) /* xzl: add more checks? */
		goto out;

	/* Now read in all of the header information */
	j = sizeof(struct elf_phdr) * elf_ex.e_phnum;
	/* j < ELF_MIN_ALIGN because elf_ex.e_phnum <= 2 */ //xzl-- matters?

	error = -ENOMEM;
	elf_phdata = (struct elf_phdr *)kmalloc(j, GFP_KERNEL);
	if (!elf_phdata)
		goto out;

	eppnt = elf_phdata;
	error = -ENOEXEC;
	retval = elf_read_img(elfbuf, elfsize, eppnt, j, elf_ex.e_phoff);
	if (retval < 0)
		goto out_free_ph;

	// xzl: @eppnt: buf for all prog headers @j: sz of all program headers
	for (i = 0; i<elf_ex.e_phnum; i++) {
		u32 start_page;
		char *addr;
		struct gpu_region *rr;

		BUG_ON(eppnt[i].p_type != PT_LOAD);

		start_page = eppnt[i].p_vaddr >> V3D_MMU_PAGE_SHIFT;

		/* the GPU mem region must have been mapped */
		rr = lookup_gpu_region(start_page);
			if (!rr) {
				printk("BUG? try to load a GPU region unmapped. start_page=%08x",
						start_page);
				goto out_free_ph;
			}
			addr = (char *)(long)(rr->phys); // phys is virt

		/* sanity check */
		if ((rr->num_pages << V3D_MMU_PAGE_SHIFT) != eppnt[i].p_memsz) {
			printk("sz %lx != memsz %llx filesz %llx",
					rr->num_pages << V3D_MMU_PAGE_SHIFT, eppnt[i].p_memsz,
					eppnt[i].p_filesz);
			goto out_free_ph;
		}

		/* map GPU BO to kernel space, upload mem contents */
		BUG_ON(!addr);
		retval = elf_read_img(elfbuf, elfsize, addr, eppnt[i].p_filesz,
				eppnt[i].p_offset/*file offset*/);

		if (retval) {
			printk("elf_read failed. why?");
			continue;
		}

//		*filesz += eppnt[i].p_filesz;

		printk("loadelf: vaddr %08llx memsz %08llx filesz %08llx",
				eppnt[i].p_vaddr, eppnt[i].p_memsz, eppnt[i].p_filesz);
	}

	error = 0;

out_free_ph:
	kfree(elf_phdata);
out:
	return error;
}
/* on success: return 0. failure: return neg */
static int v3d_replay(const struct record_entry *records,
		const char * filepath) {
	unsigned int counter = 0;
	int ret = 0;
	u64 ts_start, ts_end, total_load_ns = 0, total_irq_ns = 0, total_map_ns = 0;
	u64 total_wait_us = 0, total_waitreg_us = 0; // artificial delay, not including irq wait
	u64 total_reg_ns = 0;

//	mb();

	printk("--- replay starts --- ");

	ts_start = ktime_get_ns();

	for (;;) {
		printk("to exec: rec %u one record addr is %16lx", counter+1, (u64)records);

		switch (records->type) {
		case type_access_reg:
		{
			const char rw = records->entry_access_reg.rw;
			s64 delay = records->delay_after_us;
			int expected;
			u64 t0;

			if (delay <= REPLAY_DELAY_IRQ_BCL /*-1*/) {
				expected = atomic_read(&v3d_replay_irqcnt) + 1;
			}

			t0 = ktime_get_ns(); // can be costly?

			if (!strncmp(records->entry_access_reg.group, "hub", 3)) {
				if (rw == 'w')
					V3D_WRITE_NOTRACE(records->entry_access_reg.offset,
							records->entry_access_reg.val);
				else { /* 'r' or 'R' */
					u32 val = V3D_READ_NOTRACE(records->entry_access_reg.offset);
					if (val != records->entry_access_reg.val) { // mismatch
						if (rw == 'R') {
							printk("%d: ignore mismatched reads. actual 0x%08x expected 0x%08x. comment %s",
									counter, val, records->entry_access_reg.val,
									records->comment ? records->comment : "(n/a)");
							// return -1;  // unmatched read val
						} else if (rw == 'r') { //
							printk("%d: bad. hub mismatched reads. actual 0x%08x expected 0x%08x. comment %s",
									counter, val, records->entry_access_reg.val,
									records->comment ? records->comment : "(n/a)");
						} else {
							printk("bug? rw=%c", rw);
							ret = -2;
							goto done;
						}
					} // mismatch
				}
			} else if (!strncmp(records->entry_access_reg.group, "core", 4)) {
				BUG_ON(records->entry_access_reg.core != 0); // we only support core==0
				if (rw == 'w') {
					// XXX won't have irq without this. is it because we write reg too fast???
					printk("going to write core reg...");
					V3D_CORE_WRITE_NOTRACE(records->entry_access_reg.core,
							records->entry_access_reg.offset,
							records->entry_access_reg.val);
				} else {
					u32 val = V3D_CORE_READ_NOTRACE(records->entry_access_reg.core,
							records->entry_access_reg.offset);
					if (val != records->entry_access_reg.val) {
						if (rw == 'r')
							printk("%d: bad. core mismatched reads. actual 0x%08x expected 0x%08x. comment %s",
									counter, val, records->entry_access_reg.val,
									records->comment ? records->comment : "(n/a)");
						else if (rw == 'R')
							printk("%d: ignore core mismatched reads. actual 0x%08x expected 0x%08x. comment %s",
										counter, val, records->entry_access_reg.val,
										records->comment ? records->comment : "(n/a)");
						else {
							ret = -2;
							goto done;
						}
					}
				}
			} else {
				printk("gca or bridge? unsupported reg types.");
				ret = -1;
				goto done;
			}

			total_reg_ns += ktime_get_ns() - t0;

			if (delay > REPLAY_DELAY_BASE_US) {
				udelay(delay);
				total_wait_us += delay;
			} else if (delay == REPLAY_DELAY_WAIT_SHORT) {
				udelay(500);
				total_wait_us += 500;
			} else if (delay == REPLAY_DELAY_WAIT_NORMAL) {
				udelay(1 * 1000);		// 1ms
				total_wait_us += 1000;
			} else if (delay == REPLAY_DELAY_WAIT_LONG) {
				udelay(10 * 1000); 	// 10 ms
				total_wait_us += 10 * 1000;
			} else if (delay <= REPLAY_DELAY_IRQ_BCL /*-1*/) {
				static const char *irq_src[] = {"bcl","rcl","csd","tfu"};
				const char * src = irq_src[-delay-1];
				u64 t0, t1, ms;
				int ret;

//				expected = atomic_read(&v3d_replay_irqcnt) + 1;

				t0 = ktime_get_ns();
				// uses usleep
#if 0
				ret = (wait_for((atomic_read(&v3d_replay_irqcnt) == expected),
						WAIT_FOR_IRQ_TIMEOUT_US/1000 /*ms*/));
#else
				printk("spin waiting...");
				{ // udelay, spin waiting
					int k, cur;
					int iter = WAIT_FOR_IRQ_TIMEOUT_US / 20;
					printk("wait for irq (delay=%lld)", delay);
					for (k = 0; k < iter; k++) { // XXX spin wait. this is bad.
						cur = atomic_read(&v3d_replay_irqcnt);
						if (cur == expected)
							break;
						BUG_ON(cur > expected);
						udelay(20);
					}
					if (k == iter)
						ret = -1;
					else
						ret = 0;
				}
#endif
				t1 = ktime_get_ns();
				ms = (t1-t0)/1000/1000;
				total_irq_ns += (t1-t0);

				if (ret)
					printk("bug? wait for irq %s timeout (around %lld ms)",
							src, ms);
//					printk("bug? wait for irq %s timeout (around %lld ms) V3D_CTL_INT_STS %08x",
//							src, ms, V3D_CORE_READ(0, V3D_CTL_INT_STS));
				else
					printk("wait for irq %s okay. (around %lld ms)", src, ms);
			}	// else no delay
			break;
		}
		case type_wait_for_reg:
		{
			// alias
			const char * group = records->entry_wait_for_reg.group;
			u32 offset = records->entry_wait_for_reg.offset;
			u32 mask = records->entry_wait_for_reg.mask;
			u32 expected = records->entry_wait_for_reg.expected;
			int core = records->entry_wait_for_reg.core;

			u64 t1 = ktime_get_ns();

			if (!strncmp(group, "hub", 3)) {
				BUG_ON(core != -1);
				if (wait_for(((V3D_READ_NOREPLAY(offset) & mask) == expected),
						100 /* hard coded XXX */)) {
					printk("bad. timeout. comment %s. quit replay \n",
							records->comment ? records->comment : "(n/a)");
					ret = -1;
					goto done;
				}
			} else if (!strncmp(group, "core", 4)) {
				BUG_ON(core != 0); // we only support core==0
				if (wait_for(
						((V3D_CORE_READ_NOTRACE(core, offset) & mask) == expected),
						100 /* hard coded XXX */)) {
					printk("bad. timeout. comment %s. quit replay \n",
							records->comment ? records->comment : "(n/a)");
					ret = -1;
					goto done;
				}
			} else {
				printk("gca or bridge? unsupported reg types.");
				ret = -1;
				goto done;
			}

			total_waitreg_us += (ktime_get_ns() - t1) / 1000;
		}
		break;
		case type_map_gpu_mem:
			if (records->entry_map_gpu_mem.is_map) {
				/* map a mem region to GPU. cr a shmem obj, allocate phys mem,
				 * and set GPU ptes */
				u64 t0, t1;
				u32 start_page = records->entry_map_gpu_mem.start_page;
				u32 num_pages = records->entry_map_gpu_mem.num_pages;

				t0 = ktime_get_ns();

#if 0
				struct list_head *c;

				/* xzl: below can be costly. XXX reuse them in one replay session XXX */
				list_for_each(c, &the_shmem_freelist) {
					shmem_obj = list_entry(c, struct drm_gem_shmem_object, madv_list);
					if (shmem_obj->base.size == num_pages << V3D_MMU_PAGE_SHIFT) {
						list_del(c);
						break;
					}
				}

				if (c == &the_shmem_freelist) { /* miss in freelist, alloc a new one*/
					shmem_obj = drm_gem_shmem_create(&v3d->drm,
											num_pages << V3D_MMU_PAGE_SHIFT);
					BUG_ON(!shmem_obj);
					/* for the shmem region, allocate the underneath phys mem.
					 * the result is a sgt */
					sgt = drm_gem_shmem_get_pages_sgt(&shmem_obj->base);
					BUG_ON(!sgt);
				}
#endif

				u32 phys;
				void *p = CMemorySystem::HeapAllocate(num_pages * 4096, HEAP_DMA30);
				printk("p is %lx", (u64)p);
				BUG_ON(!p || ((u64)p & (PAGE_SIZE - 1))); // must be page aligned

				phys = (u32)(u64)p;

//				t1 = ktime_get_ns();

				/* xzl: the following is cheap */

				/* instead of alloc GPU addr via drm_mm, directly use the
				 * recorded GPU addr */
				v3d_mmu_insert_ptes(phys, start_page, num_pages);

				// bookkeep the gpu region
				{
					int ret;
					struct gpu_region * r =
							(struct gpu_region *)kmalloc(sizeof(*r), GFP_KERNEL);

					r->start_page = start_page;
					r->num_pages = num_pages;
					r->phys = phys;
					ret = add_gpu_region(start_page, r);
					if (ret) { /* 0 means okay */
						printk("gpu mem already mapped? ret %d start_page 0x%08x skip",
								ret, start_page);
						continue;
					}
				}
				printk("map GPU mem start_page %08x num_pages 0x%08x", start_page,
						num_pages);
				t1 = ktime_get_ns();
				total_map_ns += (t1-t0);
			} else {  // unmap
				u64 t0, t1;
				struct gpu_region * rr;

				t0 = ktime_get_ns();

				rr = remove_gpu_region(records->entry_map_gpu_mem.start_page);
				BUG_ON(!rr || rr->num_pages != records->entry_map_gpu_mem.num_pages);

				// return shmem to freelist. don't destroy
//				list_add(&rr->shmem->madv_list, &the_shmem_freelist);
				CMemorySystem::HeapFree((void *)(u64)(rr->phys));
				kfree(rr);

				v3d_mmu_remove_ptes(
						records->entry_map_gpu_mem.start_page,
						records->entry_map_gpu_mem.num_pages);

				t1 = ktime_get_ns();
				total_map_ns += (t1-t0);
			}
			break;
		case type_write_gpu_mem:
		{
			void *addr = NULL;
			struct gpu_region * rr = NULL;
			u32 start_page = records->entry_write_gpu_mem.start_page;
			u32 num_pages = records->entry_write_gpu_mem.sz >> V3D_MMU_PAGE_SHIFT;
			// write size must be page aligned as of now
			BUG_ON(records->entry_write_gpu_mem.sz &
					((1<<V3D_MMU_PAGE_SHIFT)-1));

			/* the GPU mem region must have been mapped */
			rr = lookup_gpu_region(start_page);
			BUG_ON(!rr || rr->num_pages != num_pages);
			addr = (void *)(u64)(rr->phys);

			printk("bo kernel CPU vaddr is %lx 0x%08x pages",
					(unsigned long)addr, num_pages);
			memcpy(addr, records->entry_write_gpu_mem.buf,
					records->entry_write_gpu_mem.sz);
			// flush? sync DMA memory?
		}
		break;
		case type_write_gpu_mem_fromfile:
#ifdef V3D_LOAD_FROM_FILE
		{
//			struct file *fp;
			FIL f;
			FRESULT fres;

			u64 t0, t1;
			ssize_t filesz;
			char * fname;

			t0 = ktime_get_ns();
			fname = (char *)kmalloc(256, GFP_KERNEL);

			BUG_ON(!fname);

			snprintf(fname, 256, "%smem_%s.elf", filepath,
					records->entry_write_gpu_mem_fromfile.tag);
//			fp = file_open(fname, O_RDONLY, 0 /* ignored */);

			printk("open file %s...", fname);

			fres = f_open(&f, fname, FA_READ | FA_OPEN_EXISTING);
			if (fres != FR_OK) {
				printk("cannot open %s for replay. quit", fname);
				ret = -1;
				kfree(fname);
				goto done;
			} else {
				xzl_load_gpu_regions_elf(&f, &filesz);
				fres = f_close(&f);
				assert(fres == FR_OK);
				kfree(fname);
				t1 = ktime_get_ns();
				printk("loaded mem dump. %zd KB in %lld ms",
						filesz/1000, (t1-t0)/1000/1000);
				total_load_ns += (t1-t0);
			}
		}
#else // load from mem image
		{
			u64 t0, t1;
			const char *elfbuf;
			ssize_t elfsize;

			extern const char _binary_mem_csd_0001_elf_start;
			extern const char _binary_mem_csd_0001_elf_end;
			extern unsigned long _binary_mem_csd_0001_elf_size;
			elfbuf = &_binary_mem_csd_0001_elf_start;
			elfsize = &_binary_mem_csd_0001_elf_end - &_binary_mem_csd_0001_elf_start;

//			const char _binary_mem_csd_0001_elf_start = 0;
//			unsigned long _binary_mem_csd_0001_elf_size = 0;

//			extern const char _binary_dummy_bin_start;
//			extern const char _binary_dummy_bin_end;
//			extern int _binary_dummy_bin_size;
//			elfbuf = &_binary_dummy_bin_start;
//			elfsize = &_binary_dummy_bin_end - &_binary_dummy_bin_start;

			t0 = ktime_get_ns();

			if (!strcmp(records->entry_write_gpu_mem_fromfile.tag, "csd_0001")) {
//				elfbuf = &_binary_mem_csd_0001_elf_start;
				printk("open elf img, base %0lx size %d...",
						elfbuf, elfsize);

				int res = xzl_load_gpu_regions_elfimg(elfbuf, elfsize);

				BUG_ON(res != 0);
				t1 = ktime_get_ns();
				printk("loaded mem dump. %lu KB in %lld ms",
						elfsize/1000, (t1-t0)/1000/1000);
				total_load_ns += (t1-t0);
			} else
				BUG();
		}
#endif
		break;
		case type_eof:
			ts_end = ktime_get_ns();
			printk("--- done. %d records replayed. %lld ms"
					"(load %lld irq %lld map %lld wait %lld waitreg %lld reg %lld)--- ",
					counter,
					(ts_end - ts_start) / 1000 / 1000,
					total_load_ns/1000/1000, total_irq_ns/1000/1000,
					total_map_ns/1000/1000, total_wait_us / 1000,
					total_waitreg_us/1000, total_reg_ns/1000/1000 );
			ret = 0;
			goto done;
		default:
			ret = -10;
			printk("unrecognized type %d, abort", records->type);
			goto done;
			break;
		} // switch

		records++;
		if (counter == MAX_RECORDS) { // failsafe.
			printk("reached # max records = %d. abort", counter);
			ret = -1;
			goto done;
		}
		counter++;
	} // for(;;)

done:
//	v3d_is_replay_xzl = 0;
//	v3d_is_recording_xzl = v3d_is_recording_saved;
//	mb();
	return ret;
}

enum irqreturn {
	IRQ_NONE		= (0 << 0),
	IRQ_HANDLED		= (1 << 0),
	IRQ_WAKE_THREAD		= (1 << 1),
};

typedef enum irqreturn irqreturn_t;


static irqreturn_t
v3d_hub_irq(void *arg)
{
	u32 intsts;
	irqreturn_t status = IRQ_NONE;

	intsts = V3D_READ(V3D_HUB_INT_STS);

	printk("xzl: a hub irq 0x%x", intsts);

	/* Acknowledge the interrupts we're handling here. */
	V3D_WRITE(V3D_HUB_INT_CLR, intsts);

	if (intsts & V3D_HUB_INT_TFUC) {
		printk("xzl: a hub tfuc irq");
		status = IRQ_HANDLED;
	}

	// xzl: mmu irq for err? no tracept. XXX: these are executed in replay
	if (intsts & (V3D_HUB_INT_MMU_WRV |
		      V3D_HUB_INT_MMU_PTI |
		      V3D_HUB_INT_MMU_CAP)) {
		u32 axi_id = V3D_READ(V3D_MMU_VIO_ID);
		int va_width = 32; // xzl: is this right?
		int ver = 42; // v3d 42?

		u64 vio_addr = ((u64)V3D_READ(V3D_MMU_VIO_ADDR) <<
				(va_width - 32));
		static const char *const v3d41_axi_ids[] = {
			"L2T",
			"PTB",
			"PSE",
			"TLB",
			"CLE",
			"TFU",
			"MMU",
			"GMP",
		};
		const char *client = "?";
		static int logged_error;

		V3D_WRITE(V3D_MMU_CTL, V3D_READ(V3D_MMU_CTL)); // xzl: why this? clear status?

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

		if (ver >= 41) {
			axi_id = axi_id >> 5;
			if (axi_id < ARRAY_SIZE(v3d41_axi_ids))
				client = v3d41_axi_ids[axi_id];
		}

		if (!logged_error)
		printk("MMU error from client %s (%d) at 0x%llx%s%s%s\n",
			client, axi_id, (long long)vio_addr,
			((intsts & V3D_HUB_INT_MMU_WRV) ?
			 ", write violation" : ""),
			((intsts & V3D_HUB_INT_MMU_PTI) ?
			 ", pte invalid" : ""),
			((intsts & V3D_HUB_INT_MMU_CAP) ?
			 ", cap exceeded" : ""));
		logged_error = 1;
		status = IRQ_HANDLED;
	}

	return status;
}


//irqreturn_t v3d_irq(void *arg)
void v3d_irq(void *arg)
{
	u32 intsts;
	int status = IRQ_NONE;
//	int irq = ARM_IRQ_V3D;

	intsts = V3D_CORE_READ(0, V3D_CTL_INT_STS);

	printk("xzl: that's an irq. V3D_CTL_INT_STS %08x", intsts);

	/* Acknowledge the interrupts we're handling here. */
	V3D_CORE_WRITE(0, V3D_CTL_INT_CLR, intsts);

	if (intsts & V3D_INT_OUTOMEM) {
			/* Note that the OOM status is edge signaled, so the
			 * interrupt won't happen again until the we actually
			 * add more memory.  Also, as of V3D 4.1, FLDONE won't
			 * be reported until any OOM state has been cleared.
			 */
			printk("xzl: overflow mem. skip");
			status = IRQ_HANDLED;
		}

		if (intsts & V3D_INT_FLDONE) { // bcl
			printk("xzl: a bcl irq");
			status = IRQ_HANDLED;
		}

		if (intsts & V3D_INT_FRDONE) {  // rcl
			printk("xzl: a rcl irq");
			status = IRQ_HANDLED;
		}

		if (intsts & V3D_INT_CSDDONE) {
			printk("xzl: a csd irq");
			status = IRQ_HANDLED;
		}

		/* We shouldn't be triggering these if we have GMP in
		 * always-allowed mode.
		 */
		if (intsts & V3D_INT_GMPV)
			printk("GMP violation\n");

		/* V3D 4.2 wires the hub and core IRQs together, so if we &
		 * didn't see the common one then check hub for MMU IRQs.
		 */
		if (status == IRQ_NONE)
			status = v3d_hub_irq(arg);

		// too many msgs
//		printk("xzl: that's all about irq. signal fence");

		atomic_inc(&v3d_replay_irqcnt);
		DataMemBarrier();
//		return status;
}

// --------------------- cv3d ------------------------ //

CV3D::CV3D (CInterruptSystem *pInterruptSystem)
: m_pInterruptSystem (pInterruptSystem)
{
}

CV3D::~CV3D (void)
{

}



void CV3D::clock_on(bool enable)
{
		CBcmPropertyTags Tags;
		TPropertyTagClockState st;

		st.nClockId = CLOCK_ID_V3D;
		st.nState = enable ? 1 : 0;

		if (!Tags.GetTag (PROPTAG_SET_CLOCK_STATE, &st, sizeof st))
			CLogger::Get()->Write (FromKernel, LogNotice, "set tag failed");
		else
			CLogger::Get()->Write (FromKernel, LogNotice, "set clock state %u", st.nState);
}

boolean CV3D::Init(void)
{

	/* test random num gen. to show reg read works */
#if 0
	CLogger::Get()->Write (FromKernel, LogNotice, "random: %08x", read32(ARM_HW_RNG200_BASE + 0x20));
	CLogger::Get()->Write (FromKernel, LogNotice, "random: %08x", read32(ARM_HW_RNG200_BASE + 0x20));
	CLogger::Get()->Write (FromKernel, LogNotice, "random: %08x", read32(ARM_HW_RNG200_BASE + 0x20));
#endif

	// check v3d clock rate
	{
		CBcmPropertyTags Tags;
		TPropertyTagClockRate r;
		r.nClockId = CLOCK_ID_V3D;

		if (!Tags.GetTag (PROPTAG_GET_CLOCK_RATE, &r, sizeof r))
			printk("get tag failed");
		else
			printk("v3d clock rate %u", r.nRate);
	}

	set_clk_states();
	dump_clk_states();

	// turn all power domains on via firmware. this seems not enough
	dump_pd_states();
	set_pd_states();
	dump_pd_states();

	dump_v3d_regs();

	// will have to turn v3d on via asb, as did in linux
	dump_asb_regs();
	power_on();

	// read reg again
	dump_v3d_regs();

	pt_paddr = (u32 *)CMemorySystem::Get()->HeapAllocate(1024 * 4096, HEAP_DMA30);
	mmu_scratch_paddr = CMemorySystem::Get()->HeapAllocate(4096, HEAP_DMA30);


	if (pt_paddr && mmu_scratch_paddr)
		printk("pgtable %08x mmu_scratch_paddr %08x", (u64)pt_paddr,
				(u64)mmu_scratch_paddr);
	else
		printk("bug? failed to alloc");

//	printk("pgtable %08x mmu_scratch_paddr %08x", (u64)pt_paddr & (PAGE_SIZE-1),
//			(u64)mmu_scratch_paddr & (PAGE_SIZE-1));

	assert(pt_paddr && mmu_scratch_paddr
			&& (((u64)pt_paddr & (PAGE_SIZE-1)) == 0)
			&& (((u64)mmu_scratch_paddr & (PAGE_SIZE-1)) == 0)
			);

	// cf: v3d_mmu_set_page_table()
	V3D_WRITE(V3D_MMU_PT_PA_BASE, (u64)(pt_paddr) >> V3D_MMU_PAGE_SHIFT);
	V3D_WRITE(V3D_MMU_CTL,
		  V3D_MMU_CTL_ENABLE |
		  V3D_MMU_CTL_PT_INVALID_ENABLE |
		  V3D_MMU_CTL_PT_INVALID_ABORT |
		  V3D_MMU_CTL_PT_INVALID_INT |
		  V3D_MMU_CTL_WRITE_VIOLATION_ABORT |
		  V3D_MMU_CTL_WRITE_VIOLATION_INT |
		  V3D_MMU_CTL_CAP_EXCEEDED_ABORT |
		  V3D_MMU_CTL_CAP_EXCEEDED_INT);
			V3D_WRITE(V3D_MMU_ILLEGAL_ADDR,
		  (u64)(mmu_scratch_paddr) >> V3D_MMU_PAGE_SHIFT |
		  V3D_MMU_ILLEGAL_ADDR_ENABLE);
	V3D_WRITE(V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_ENABLE);

	v3d_mmu_flush_all();

	printk("----- connect irq ----- ");
	assert(m_pInterruptSystem);
//	m_pInterruptSystem->DisconnectIRQ(ARM_IRQ_V3D); // why it was connected already??
	m_pInterruptSystem->ConnectIRQ(ARM_IRQ_V3D, v3d_irq, 0);

	return true;
}

/*
 cf: bcm2835_asb_power_on
*/
void CV3D::asb_power_on(void)
{
	u32 pm_reg = PM_GRAFX;
	u32 asb_m_reg = ASB_V3D_M_CTRL;
	u32 asb_s_reg = ASB_V3D_S_CTRL;
	u32 reset_flags = PM_V3DRSTN;

	// other modules
//	u32 pm_reg = PM_IMAGE;
//	u32 asb_m_reg = ASB_ISP_S_CTRL;
//	u32 asb_s_reg = ASB_ISP_M_CTRL;
//	u32 reset_flags = PM_ISPRSTN;

//		asb_m_reg = ASB_H264_S_CTRL;
//		asb_s_reg = ASB_H264_M_CTRL;

	// in fact, enable/disable clock below may be unnecessary. the kernel's driver
	// for enabling/disabling the v3d clock
	clock_on(true);
	CTimer::Get()->usDelay(1);
	clock_on(false);


//	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~reset_flags);

	printk("before reset %08x", PM_READ(pm_reg)); // should be 0000_1000
	/* Deassert the resets. */
	PM_WRITE(pm_reg, PM_READ(pm_reg) | reset_flags);
	printk("after reset %08x", PM_READ(pm_reg)); // should be 0000_1040

	clock_on(true);

	CTimer::Get()->usDelay(1);

	// master
	int ret = asb_enable(asb_m_reg);
	if (!ret)
		printk("enable master ok");
	else
		printk("enable master failed. ret %d", ret);

	// slave
	ret = asb_enable(asb_s_reg);
	if (!ret)
		printk("enable slave ok");
	else
		printk("enable slave failed. ret %d", ret);
}

void CV3D::asb_power_off(void)
{
	u32 pm_reg = PM_GRAFX;
	u32 asb_m_reg = ASB_V3D_M_CTRL;
	u32 asb_s_reg = ASB_V3D_S_CTRL;
	u32 reset_flags = PM_V3DRSTN;

	int ret = asb_disable(asb_m_reg);
	if (!ret)
		CLogger::Get()->Write (FromKernel, LogNotice, "disable master ok");
	else
		CLogger::Get()->Write (FromKernel, LogNotice, "disable master failed. ret %d", ret);

	// slave
	ret = asb_disable(asb_s_reg);
	if (!ret)
		CLogger::Get()->Write (FromKernel, LogNotice, "disable slave ok");
	else
		CLogger::Get()->Write (FromKernel, LogNotice, "disable slave failed. ret %d", ret);

	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~reset_flags);
}


void CV3D::power_on(bool on)
{
	if (on)
		asb_power_on();
	else
		asb_power_off();
}

extern "C"{
	extern struct record_entry v3d_records_loadmem[]; // v3d_replay_linux.c
	extern struct record_entry v3d_records_py[];
}

int CV3D::Replay(void)
{
//	printk("CV3D::Replay addr is %16lx", (u64)&v3d_records_loadmem);
//	v3d_replay(&v3d_records_loadmem[0], "");
	v3d_replay(&v3d_records_py[0], "");
	v3d_replay_cleanup();

	zlib_inflate_blob(NULL, 0, NULL, 0); // try to call it XXX
	return 0;
}
