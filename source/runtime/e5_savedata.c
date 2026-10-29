/* e5_savedata.c - libSceSaveData (+ dialog, temporary data) backed by host folders.
 * Saves live in "savedata/<dirName>" in the current folder (never in the game folder); mounted at
 * "/savedata0".. so the game's own sceKernelOpen/Write calls land there. Included by e3_hle.c. */

static const char *g_save_root = "savedata";

static void save_host_dir(char *out, const char *dir) { path_join(out, g_save_root, dir); }
static void copy_n(char *d, const char *s, int n) { int i = 0; if (s) for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }

/* PS4 search patterns use '%' as "any sequence" */
static int pat_match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '%') { for (;;) { if (pat_match(p + 1, s)) return 1; if (!*s) return 0; s++; } }
    return *p == *s && pat_match(p + 1, s + 1);
}

typedef struct { u32 hit_num, pad0; char (*names)[32]; u32 names_num, set_num; void *params, *infos; } SdSearchResult;
typedef struct { int user; int pad; const char *title_id; const char *dir_name; u32 key, order; } SdSearchCond;
typedef struct { SdSearchResult *r; const char *pat; } SdSearchCtx;
static void sd_search_cb(const char *name, void *c_) {
    SdSearchCtx *c = (SdSearchCtx *)c_;
    if (c->pat && c->pat[0] && !pat_match(c->pat, name)) return;
    if (c->r->names && c->r->set_num < c->r->names_num) copy_n(c->r->names[c->r->set_num++], name, 32);
    c->r->hit_num++;
}
static SYSV int sd_dir_search(const SdSearchCond *cond, SdSearchResult *r) {
    TRACE("sceSaveDataDirNameSearch", cond, r, cond ? cond->dir_name : 0);
    if (!cond || !r) return (int)0x809F0001;
    r->hit_num = 0; r->set_num = 0;
    SdSearchCtx c = {r, cond->dir_name};
    pm_lock(&g_save_lock); plat_list_dirs(g_save_root, sd_search_cb, &c); pm_unlock(&g_save_lock);
    return 0;
}

typedef struct { int user; int pad; const char *title_id; const char *dir_name; const void *fingerprint; u64 blocks; u32 mode; } SdMount;
typedef struct { char mount_point[16]; u64 required_blocks; u32 unused, mount_status; } SdMountResult;
static int g_save_mounts_total;
static SYSV int sd_mount(const SdMount *m, SdMountResult *r) {
    TRACE("sceSaveDataMount", m ? m->dir_name : 0, m ? m->mode : 0, m ? m->blocks : 0);
    if (!m || !r || !m->dir_name) return (int)0x809F0001;
    char host[1100]; save_host_dir(host, m->dir_name);
    pm_lock(&g_save_lock);
    int exists = plat_is_dir(host), status = 0;
    if (!exists) {
        if (!(m->mode & (4 | 0x20))) { pm_unlock(&g_save_lock); return (int)0x809F0008; }   /* NOT_FOUND */
        plat_mkdir(g_save_root); plat_mkdir(host);
        { char sys[1100]; path_join(sys, host, "sce_sys"); plat_mkdir(sys); }
        status = 1;                                                                           /* CREATED */
    } else if ((m->mode & 4) && !(m->mode & 0x20)) { pm_unlock(&g_save_lock); return (int)0x809F0007; }   /* EXISTS */
    int slot = -1;
    for (int k = 0; k < 16 && slot < 0; k++) {
        char vp[16] = "/savedata"; int n = 9; if (k >= 10) vp[n++] = '1'; vp[n++] = (char)('0' + k % 10); vp[n] = 0;
        int used = 0; for (int i = 0; i < 8; i++) if (g_wm[i].used && str_eq(g_wm[i].vpath, vp)) used = 1;
        if (!used) { slot = k; wmount_add(vp, host); copy_n(r->mount_point, vp, 16); }
    }
    pm_unlock(&g_save_lock);
    if (slot < 0) return (int)0x809F000B;   /* MOUNT_FULL */
    r->required_blocks = 0; r->mount_status = (u32)status;
    g_save_mounts_total++;
    tr("    save data \""); tr(m->dir_name); tr(status ? "\" created and mounted at " : "\" mounted at "); tr(r->mount_point); tr("\n");
    return 0;
}
static SYSV int sd_umount(const char *mp) { TRACE("sceSaveDataUmount", mp, 0, 0); if (mp) { pm_lock(&g_save_lock); wmount_del(mp); pm_unlock(&g_save_lock); } return 0; }
static SYSV int sd_umount_backup(const char *mp) { TRACE("sceSaveDataUmountWithBackup", mp, 0, 0); return sd_umount(mp); }

static int sd_param_path(char *out, const char *mp, u32 type) {
    char base[1100];
    if (map_path(base, mp) != 2) return 0;
    char nm[32] = "sce_sys/param_0.bin"; nm[14] = (char)('0' + (type % 10));
    path_join(out, base, nm);
    return 1;
}
static SYSV int sd_set_param(const char *mp, u32 type, const void *buf, u64 size) {
    TRACE("sceSaveDataSetParam", mp, type, size);
    char p[1100]; if (!sd_param_path(p, mp, type)) return (int)0x809F0009;   /* NOT_MOUNTED */
    void *f = hf_open_rw(p, 1, 1); if (!f) return (int)0x809F0001;
    lz_ensure((u64)buf, size); hf_write(f, buf, size); hf_close(f);
    return 0;
}
static SYSV int sd_get_param(const char *mp, u32 type, void *buf, u64 size, u64 *got) {
    TRACE("sceSaveDataGetParam", mp, type, size);
    char p[1100]; if (!sd_param_path(p, mp, type)) return (int)0x809F0009;
    h_memset(buf, 0, size);
    void *f = hf_open_rb(p); u64 n = 0;
    if (f) { lz_ensure((u64)buf, size); n = hf_read(f, buf, size); hf_close(f); }
    if (got) *got = n;
    return 0;
}
static SYSV int sd_check_backup(const void *chk) { TRACE("sceSaveDataCheckBackupData", chk, 0, 0); return (int)0x809F0008; }   /* no backups */
static SYSV int sd_delete(const SdMount *d) { TRACE("sceSaveDataDelete", d ? d->dir_name : 0, 0, 0); return 0; }
static SYSV int sd_ok(void) { return 0; }

/* save-data dialog: completes immediately (no UI yet) */
static int g_sd_dialog_status;   /* 0 NONE, 1 INITIALIZED, 2 RUNNING, 3 FINISHED */
static SYSV int sdd_init(void) { TRACE("sceSaveDataDialogInitialize", 0, 0, 0); g_sd_dialog_status = 1; return 0; }
static SYSV int sdd_open(const void *p) { TRACE("sceSaveDataDialogOpen", p, 0, 0); g_sd_dialog_status = 3; return 0; }
static SYSV int sdd_status(void) { return g_sd_dialog_status; }
static SYSV int sdd_close(void) { g_sd_dialog_status = 3; return 0; }
static SYSV int sdd_term(void) { g_sd_dialog_status = 0; return 0; }
typedef struct { u32 mode; int result; u32 button_id; } SddResult;
static SYSV int sdd_result(SddResult *r) { TRACE("sceSaveDataDialogGetResult", r, 0, 0); if (r) { r->result = 0; r->button_id = 1; } return 0; }

/* temporary data (/temp0) */
typedef struct { char data[16]; } MountPoint16;
static SYSV int tmp_mount2(u32 option, MountPoint16 *mp) {
    TRACE("sceAppContentTemporaryDataMount2", option, mp, 0);
    plat_mkdir("temp0");
    wmount_add("/temp0", "temp0");
    if (mp) copy_n(mp->data, "/temp0", 16);
    return 0;
}
static SYSV int tmp_space(const MountPoint16 *mp, u64 *kb) { (void)mp; if (kb) *kb = 1024ull * 1024; return 0; }

static const HleEntry g_hle_save[] = {
    {"sceSaveDataInitialize", (void *)sd_ok}, {"sceSaveDataInitialize2", (void *)sd_ok}, {"sceSaveDataInitialize3", (void *)sd_ok},
    {"sceSaveDataTerminate", (void *)sd_ok},
    {"sceSaveDataDirNameSearch", (void *)sd_dir_search}, {"sceSaveDataMount", (void *)sd_mount}, {"sceSaveDataMount2", (void *)sd_mount},
    {"sceSaveDataUmount", (void *)sd_umount}, {"sceSaveDataUmountWithBackup", (void *)sd_umount_backup},
    {"sceSaveDataSetParam", (void *)sd_set_param}, {"sceSaveDataGetParam", (void *)sd_get_param},
    {"sceSaveDataCheckBackupData", (void *)sd_check_backup}, {"sceSaveDataDelete", (void *)sd_delete},
    {"sceSaveDataSaveIcon", (void *)sd_ok}, {"sceSaveDataRestoreBackupData", (void *)sd_ok},
    {"sceSaveDataDialogInitialize", (void *)sdd_init}, {"sceSaveDataDialogOpen", (void *)sdd_open},
    {"sceSaveDataDialogUpdateStatus", (void *)sdd_status}, {"sceSaveDataDialogGetStatus", (void *)sdd_status},
    {"sceSaveDataDialogClose", (void *)sdd_close}, {"sceSaveDataDialogTerminate", (void *)sdd_term},
    {"sceSaveDataDialogGetResult", (void *)sdd_result}, {"sceSaveDataDialogProgressBarSetValue", (void *)sd_ok},
    {"sceAppContentTemporaryDataMount2", (void *)tmp_mount2}, {"sceAppContentTemporaryDataGetAvailableSpaceKb", (void *)tmp_space},
    {0, 0}};
