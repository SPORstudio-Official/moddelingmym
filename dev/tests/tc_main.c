int hal_init(void); void irq_init(void); void mm_init(void); int vfs_mount(void); void cc_init(void);
void hal_shutdown(void); const char* klog_data(unsigned*);
extern int cc_debug_dump;
int main(int argc, char **argv) {
    hal_init(); irq_init(); mm_init(); vfs_mount(); cc_init();
    if (argc < 2) return 1;
    cc_debug_dump = (argc > 2);
    FILE *f = fopen(argv[1], "rb");
    static char src[8192];
    int n = (int)fread(src, 1, sizeof(src) - 1, f);
    src[n] = 0;
    fclose(f);
    static char out[8192];
    int rc = cc_run_source(src, "test", out, (argc > 3) ? 300u : (unsigned)sizeof(out), NULL, NULL, 0);
    fprintf(stderr, "RC=%d\nOUTPUT>>>\n%s<<<OUTPUT\n", rc, out);
    if (argc > 2) {
        unsigned ll = 0;
        const char *lg = klog_data(&ll);
        fprintf(stderr, "%.*s\n", (int)ll, lg);
    }
    return rc;
}
