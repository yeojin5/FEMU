/**
 * Filename: dftl.c
 * Author: Zihang Lin
 * Date Created: 2022-05
 * Version: 1.0
 *
 * Brief Description:
 *     This file implementing the main function of DFTL (ASPLOS' 09).
 *
 * Copyright Notice:
 *     Copyright (C) 2023 Zihang Lin. All rights reserved.
 *     License: Distributed under the GPL License.
 *
 */

#include "dftl.h"

//#define FEMU_DEBUG_FTL

static void *ftl_thread(void *arg);

/* process hash */
static inline uint64_t cmt_hash(uint64_t lpn)
{
    return lpn % CMT_HASH_SIZE;
}

static struct cmt_entry* find_hash_entry(hash_table *ht, uint64_t lpn)
{
    uint64_t pos = cmt_hash(lpn);
    cmt_entry *entry = ht->cmt_table[pos];
    while (entry != NULL && entry->lpn != lpn) {
        entry = entry->next;
    }
    return entry;
}

static void insert_cmt_hashtable(hash_table *ht, cmt_entry *entry) 
{
    uint64_t pos = cmt_hash(entry->lpn);
    entry->next = ht->cmt_table[pos];
    ht->cmt_table[pos] = entry;
}

static bool delete_cmt_hashnode(hash_table *ht, cmt_entry *entry)
{
    uint64_t pos = cmt_hash(entry->lpn);
    cmt_entry *tmp_entry = ht->cmt_table[pos], *pre_entry;
    if (tmp_entry == entry) {
        ht->cmt_table[pos] = tmp_entry->next;
        tmp_entry->next = NULL;
    } else {
        pre_entry = tmp_entry;
        tmp_entry = tmp_entry->next;
        while (tmp_entry != NULL && tmp_entry != entry) {
            pre_entry = tmp_entry;
            tmp_entry = tmp_entry->next;
        }
        if (tmp_entry == NULL)
            return false;
        pre_entry->next = tmp_entry->next;
        tmp_entry->next = NULL;
    }
    return true;
}

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static inline struct ppa get_gtd_ent(struct ssd *ssd, uint64_t tvpn)
{
    return ssd->gtd[tvpn];
}

static inline void set_gtd_ent(struct ssd *ssd, uint64_t tvpn, struct ppa *ppa)
{
    ftl_assert(tvpn < ssd->sp.tt_gtd_size);
    ssd->gtd[tvpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->curline->type = DATA;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
}

static void ssd_init_trans_write_pointer(struct ssd *ssd)
{
    struct trans_write_pointer *twpp = &ssd->twp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    twpp->curline = curline;
    twpp->curline->type = TRANS;
    twpp->ch = 0;
    twpp->lun = 0;
    twpp->pg = 0;
    twpp->blk = curline->id;
    twpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->curline->type = DATA;
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static void ssd_advance_trans_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct trans_write_pointer *twpp = &ssd->twp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(twpp->ch, spp->nchs);
    twpp->ch++;
    if (twpp->ch == spp->nchs) {
        twpp->ch = 0;
        check_addr(twpp->lun, spp->luns_per_ch);
        twpp->lun++;
        /* in this case, we should go to next lun */
        if (twpp->lun == spp->luns_per_ch) {
            twpp->lun = 0;
            /* go to next page in the block */
            check_addr(twpp->pg, spp->pgs_per_blk);
            twpp->pg++;
            if (twpp->pg == spp->pgs_per_blk) {
                twpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (twpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(twpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, twpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(twpp->curline->vpc >= 0 && twpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(twpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, twpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(twpp->blk, spp->blks_per_pl);
                twpp->curline = NULL;
                twpp->curline = get_next_free_line(ssd);
                if (!twpp->curline) {
                    /* TODO */
                    abort();
                }
                twpp->curline->type = TRANS;
                twpp->blk = twpp->curline->id;
                check_addr(twpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(twpp->pg == 0);
                ftl_assert(twpp->lun == 0);
                ftl_assert(twpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(twpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static struct ppa get_new_trans_page(struct ssd *ssd)
{
    struct trans_write_pointer *twpp = &ssd->twp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = twpp->ch;
    ppa.g.lun = twpp->lun;
    ppa.g.pg = twpp->pg;
    ppa.g.blk = twpp->blk;
    ppa.g.pl = twpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 256;
    spp->blks_per_pl = 256; /* 16GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;

    spp->ents_per_pg = 512;
    spp->tt_gtd_size = spp->tt_pgs / spp->ents_per_pg;
    spp->tt_cmt_size = spp->tt_blks / 2;

    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

static void ssd_init_gtd(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->gtd = g_malloc0(sizeof(struct ppa) * spp->tt_gtd_size);
    for (int i = 0; i < spp->tt_gtd_size; i++) {
        ssd->gtd[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_cmt(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct cmt_mgmt *cm = &ssd->cm;
    struct cmt_entry *cmt_entry;
    struct hash_table *ht = &cm->ht;

    cm->tt_entries = spp->tt_cmt_size;
    cm->free_cmt_entry_cnt = 0;
    cm->used_cmt_entry_cnt = 0;

    cm->cmt_entries = g_malloc0(sizeof(struct cmt_entry) * cm->tt_entries);
    QTAILQ_INIT(&cm->free_cmt_entry_list);
    QTAILQ_INIT(&cm->cmt_entry_list);

    for (int i = 0; i < spp->tt_cmt_size; i++) {
        cmt_entry = &cm->cmt_entries[i];
        cmt_entry->dirty = CLEAN;
        cmt_entry->lpn = INVALID_LPN;
        cmt_entry->ppn = UNMAPPED_PPA;
        /* hash table */
        cmt_entry->next = NULL;

        QTAILQ_INSERT_TAIL(&cm->free_cmt_entry_list, cmt_entry, entry);
        cm->free_cmt_entry_cnt++;
    }
    ftl_assert(cm->free_cmt_entry_cnt == cm->tt_entries);

    for (int i = 0; i < CMT_HASH_SIZE; i++) {
        ht->cmt_table[i] = NULL;
    }
}

static void ssd_init_statistics(struct ssd *ssd)
{
    struct statistics *st = &ssd->stat;

    st->cmt_hit_cnt = 0;
    st->cmt_miss_cnt = 0;
    st->cmt_hit_ratio = 0;
    st->access_cnt = 0;
}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize gtd and cmt */
    ssd_init_gtd(ssd);
    ssd_init_cmt(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);
    ssd_init_trans_write_pointer(ssd);

    ssd_init_statistics(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static struct cmt_entry *cmt_hit(struct ssd *ssd, uint64_t lpn)
{
    struct cmt_mgmt *cm = &ssd->cm;
    struct cmt_entry *cmt_entry = NULL;
    struct hash_table *ht = &cm->ht;

    cmt_entry = find_hash_entry(ht, lpn);
    if (cmt_entry != NULL) {
        QTAILQ_REMOVE(&cm->cmt_entry_list, cmt_entry, entry);
        QTAILQ_INSERT_HEAD(&cm->cmt_entry_list, cmt_entry, entry);
    }

    return cmt_entry;
}

static uint64_t translation_page_read(struct ssd *ssd, NvmeRequest *req, struct ppa *ppa)
{
    uint64_t lat = 0;
    struct nand_cmd trd;
    trd.type = USER_IO;
    trd.cmd = NAND_READ;
    trd.stime = req->stime;
    lat = ssd_advance_status(ssd, ppa, &trd);
    
    return lat;
}

static inline uint64_t translation_page_read_no_req(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t lat = 0;
    struct nand_cmd trd;
    trd.type = USER_IO;
    trd.cmd = NAND_READ;
    trd.stime = 0;
    lat = ssd_advance_status(ssd, ppa, &trd);
    
    return lat;
}

static uint64_t translation_page_write(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    // struct nand_lun *new_lun, *old_lun;
    uint64_t tvpn = get_rmap_ent(ssd, old_ppa);
    uint64_t lat = 0;

    if (mapped_ppa(old_ppa)) {
        /* update old page information first */
        mark_page_invalid(ssd, old_ppa);
        set_rmap_ent(ssd, INVALID_LPN, old_ppa);
    }
    new_ppa = get_new_trans_page(ssd);
    /* after reading the translation page, new translation page can begin to write */
    // old_lun = get_lun(ssd, old_ppa);
    // new_lun = get_lun(ssd, &new_ppa);
    // new_lun->next_lun_avail_time = (old_lun->next_lun_avail_time > new_lun->next_lun_avail_time) ? 
    //                             old_lun->next_lun_avail_time : new_lun->next_lun_avail_time;
    /* update maptbl */
    set_gtd_ent(ssd, tvpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, tvpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_trans_write_pointer(ssd);

    struct nand_cmd twr;
    twr.type = USER_IO;
    twr.cmd = NAND_WRITE;
    twr.stime = 0;
    lat = ssd_advance_status(ssd, &new_ppa, &twr);

    return lat;
}

static uint64_t translation_page_new_write(struct ssd *ssd, uint64_t tvpn)
{
    struct ppa new_ppa;
    uint64_t lat = 0;

    new_ppa = get_new_trans_page(ssd);
    /* update maptbl */
    set_gtd_ent(ssd, tvpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, tvpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_trans_write_pointer(ssd);

    struct nand_cmd twr;
    twr.type = USER_IO;
    twr.cmd = NAND_WRITE;
    twr.stime = 0;
    lat = ssd_advance_status(ssd, &new_ppa, &twr);

    return lat;
}

static void insert_entry_to_cmt(struct ssd *ssd, uint64_t lpn, uint64_t ppn)
{
    struct cmt_mgmt *cm = &ssd->cm;
    struct hash_table *ht = &cm->ht;
    struct cmt_entry *cmt_entry;

    cmt_entry = QTAILQ_FIRST(&cm->free_cmt_entry_list);
    if (cmt_entry == NULL) {
        ftl_err("no cmt entry in free cmt entry list!");
    }
    QTAILQ_REMOVE(&cm->free_cmt_entry_list, cmt_entry, entry);
    cm->free_cmt_entry_cnt--;
    cmt_entry->lpn = lpn;
    cmt_entry->ppn = ppn;
    cmt_entry->dirty = CLEAN;
    cmt_entry->next = NULL;

    QTAILQ_INSERT_HEAD(&cm->cmt_entry_list, cmt_entry, entry);
    cm->used_cmt_entry_cnt++;
    insert_cmt_hashtable(ht, cmt_entry);
}

static void evict_entry_from_cmt(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct cmt_mgmt *cm = &ssd->cm;
    struct cmt_entry *cmt_entry;
    uint64_t tvpn;
    struct ppa ppa;
    struct hash_table *ht = &cm->ht;

    cmt_entry = QTAILQ_LAST(&cm->cmt_entry_list);
    /* evict entry in CMT */
    QTAILQ_REMOVE(&cm->cmt_entry_list, cmt_entry, entry);

    if (cmt_entry->dirty == DIRTY) {
        tvpn = cmt_entry->lpn / spp->ents_per_pg;
        ppa = get_gtd_ent(ssd, tvpn);
        /* if no correspond translation page, write new page, else, 
        read old translation page and then write new page */
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            translation_page_new_write(ssd, tvpn);
        } else {
            /* old translation page read */
            translation_page_read_no_req(ssd, &ppa);
            /* update translation page and write */
            /* delay the GTD update until "write" happens */
            translation_page_write(ssd, &ppa);
        }
    }

    if (!delete_cmt_hashnode(ht, cmt_entry)) {
        printf("error, removed entry is not in hash table!");
    }

    /* insert evicted entry to free_entry_list */
    cmt_entry->dirty = CLEAN;
    cmt_entry->lpn = INVALID_LPN;
    cmt_entry->ppn = UNMAPPED_PPA;
    QTAILQ_INSERT_TAIL(&cm->free_cmt_entry_list, cmt_entry, entry);
    cm->free_cmt_entry_cnt++;
    cm->used_cmt_entry_cnt--;
}

static struct nand_lun *process_translation_page_read(struct ssd *ssd, NvmeRequest *req, uint64_t lpn)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    struct cmt_mgmt *cm = &ssd->cm;
    uint64_t tvpn, ppn;
    struct nand_lun *lunp;

    tvpn = lpn / spp->ents_per_pg;
    ppa = get_gtd_ent(ssd, tvpn);
    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
        //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
        //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
        //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
        return NULL;
    }
    //translation page read latency
    translation_page_read(ssd, req, &ppa);
    lunp = get_lun(ssd, &ppa);
    //get real lpn-ppn
    ppa = get_maptbl_ent(ssd, lpn);
    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
        return NULL;
    } else {
        ppn = ppa2pgidx(ssd, &ppa);
        if (cm->used_cmt_entry_cnt < cm->tt_entries) {
            insert_entry_to_cmt(ssd, lpn, ppn);
        } else if (cm->used_cmt_entry_cnt == cm->tt_entries) {
            evict_entry_from_cmt(ssd);
            insert_entry_to_cmt(ssd, lpn, ppn);
        } else {
            ftl_err("wrong! cmt used entries exceed total entries!");
        }
    }

    return lunp;
}

static struct nand_lun *process_translation_page_write(struct ssd *ssd, NvmeRequest *req, uint64_t lpn)
{
    struct ssdparams *spp = &ssd->sp;
    struct ppa ppa;
    struct cmt_mgmt *cm = &ssd->cm;
    // struct cmt_entry *cmt_entry;
    uint64_t tvpn, ppn;
    struct nand_lun *lunp;

    //get gtd mapping physical page
    tvpn = lpn / spp->ents_per_pg;
    ppa = get_gtd_ent(ssd, tvpn);

    /* if it is a new write, not an update */
    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
        if (cm->used_cmt_entry_cnt < cm->tt_entries) {
            insert_entry_to_cmt(ssd, lpn, UNMAPPED_PPA);
        } else if (cm->used_cmt_entry_cnt == cm->tt_entries) {
            evict_entry_from_cmt(ssd);
            insert_entry_to_cmt(ssd, lpn, UNMAPPED_PPA);
        } else {
            ftl_err("wrong! cmt used entries exceed total entries!");
        }
        return NULL;
    }
    //read latency
    translation_page_read(ssd, req, &ppa);
    lunp = get_lun(ssd, &ppa);
    //get real lpn-ppn
    ppa = get_maptbl_ent(ssd, lpn);

    if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
        if (cm->used_cmt_entry_cnt < cm->tt_entries) {
            insert_entry_to_cmt(ssd, lpn, UNMAPPED_PPA);
        } else if (cm->used_cmt_entry_cnt == cm->tt_entries) {
            evict_entry_from_cmt(ssd);
            insert_entry_to_cmt(ssd, lpn, UNMAPPED_PPA);
        } else {
            ftl_err("wrong! cmt used entries exceed total entries!");
        }
    } else {
        ppn = ppa2pgidx(ssd, &ppa);
        if (cm->used_cmt_entry_cnt < cm->tt_entries) {
            insert_entry_to_cmt(ssd, lpn, ppn);
        } else if (cm->used_cmt_entry_cnt == cm->tt_entries) {
            evict_entry_from_cmt(ssd);
            insert_entry_to_cmt(ssd, lpn, ppn);
        } else {
            ftl_err("wrong! cmt used entries exceed total entries!");
        }
    }

    return lunp;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static uint64_t gc_translation_page_write(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t tvpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, tvpn));
    new_ppa = get_new_trans_page(ssd);
    /* update GTD */
    set_gtd_ent(ssd, tvpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, tvpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_trans_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_data_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    uint64_t lpn, tvpn;
    struct cmt_entry *cmt_entry;
    struct ppa new_ppa;
    uint64_t batch_update[spp->pgs_per_blk];
    int pos = 0, flag;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            lpn = get_rmap_ent(ssd, ppa);
            if (ppa2pgidx(ssd, ppa) != ppa2pgidx(ssd, &ssd->maptbl[lpn])) {
                printf("data block contains translation page!\n");
            } else {
                /* delay the maptbl update until "write" happens */
                gc_write_page(ssd, ppa);
                cmt_entry = find_hash_entry(&ssd->cm.ht, lpn);
                if (cmt_entry) {
                    new_ppa = get_maptbl_ent(ssd, lpn);
                    cmt_entry->ppn = ppa2pgidx(ssd, &new_ppa);
                    cmt_entry->dirty = DIRTY;
                } else {
                    flag = 0;
                    tvpn = lpn / spp->ents_per_pg;
                    for (int i = 0; i < pos; i++) {
                        if (batch_update[i] == tvpn) {
                            flag = 1;
                            break;
                        }
                    }
                    if (!flag) {
                        batch_update[pos++] = tvpn;
                        new_ppa = get_gtd_ent(ssd, tvpn);
                        translation_page_read_no_req(ssd, &new_ppa);
                        translation_page_write(ssd, &new_ppa);
                    }
                }
            }
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void clean_one_trans_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    uint64_t lpn;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            lpn = get_rmap_ent(ssd, ppa);
            if (ppa2pgidx(ssd, ppa) != ppa2pgidx(ssd, &ssd->maptbl[lpn])) {
                gc_translation_page_write(ssd, ppa);
            } else {
                printf("translation block contains data page!\n");
            }
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    line->type = NONE;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            if (victim_line->type == DATA) {
                clean_one_data_block(ssd, &ppa);
            } else if (victim_line->type == TRANS) {
                clean_one_trans_block(ssd, &ppa);
            } else {
                printf("Wrong victim line!\n");
            }
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    struct nand_lun *old_lun, *lun;
    struct statistics *st = &ssd->stat;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        st->access_cnt++;
        if (cmt_hit(ssd, lpn)) {
            st->cmt_hit_cnt++;
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }
        } else {
            st->cmt_miss_cnt++;
            old_lun = process_translation_page_read(ssd, req, lpn);
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }
            lun = get_lun(ssd, &ppa);
            lun->next_lun_avail_time = (old_lun->next_lun_avail_time > lun->next_lun_avail_time) ? \
                                        old_lun->next_lun_avail_time : lun->next_lun_avail_time;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;
    struct cmt_entry *cmt_entry;
    struct statistics *st = &ssd->stat;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        st->access_cnt++;
        cmt_entry = cmt_hit(ssd, lpn);
        if (cmt_entry) {
            st->cmt_hit_cnt++;
        } else {
            st->cmt_miss_cnt++;
            process_translation_page_write(ssd, req, lpn);
        }
        ppa = get_maptbl_ent(ssd, lpn);

        cmt_entry = find_hash_entry(&ssd->cm.ht, lpn);
        if (cmt_entry == NULL) {
            ftl_err("after process translation page, there is still no entry in cmt");
        }

        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update cmt */
        cmt_entry->ppn = ppa2pgidx(ssd, &ppa);
        cmt_entry->dirty = DIRTY;
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

