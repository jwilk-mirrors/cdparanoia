extern int verbose;
extern int quiet;

#define report(...) if(!quiet){fprintf(stderr, __VA_ARGS__);fputc('\n',stderr);}
#define reportC(...) if(!quiet){fprintf(stderr, __VA_ARGS__);}
