extern int scsi_enable_cdda(cdrom_drive *,int);
extern long scsi_read_mmc(cdrom_drive *,void *,long,long);
extern long scsi_read_mmc2(cdrom_drive *,void *,long,long);
extern long scsi_read_D4(cdrom_drive *,void *,long,long);
extern long scsi_read_D8(cdrom_drive *,void *,long,long);
extern long scsi_read_28(cdrom_drive *,void *,long,long);
extern long scsi_read_A8(cdrom_drive *,void *,long,long);

typedef struct exception {
  char *model;
  int atapi; /* If the ioctl doesn't work */
  unsigned char density;
  int  (*enable)(struct cdrom_drive *,int);
  long (*read)(struct cdrom_drive *,void *,long,long);
  int  bigendianp;
  int  ignore_toc_offset;
} exception;

/* specific to general */

/* list of drives that affect autosensing in ATAPI specific portions of code 
   (force drives to detect as ATAPI or SCSI, force ATAPI read command */

static exception atapi_list[]={
  {"TOSHIBA CD-ROM XM-5702B",       -1,0x82,         Dummy,          NULL,0,1},
  {"SAMSUNG SCR-830 REV 2.09 2.09 ", 1,   0,         Dummy,scsi_read_mmc2,0,0},
  {"Memorex CR-622",                 1,   0,         Dummy,          NULL,0,0},
  {NULL,0,0,NULL,NULL,0}};

/* list of drives that affect MMC default settings */

static exception mmc_list[]={
  {"SAMSUNG SCR-830 REV 2.09 2.09 ", 1,   0,         Dummy,scsi_read_mmc2,0,0},
  {"Memorex CR-622",                 1,   0,         Dummy,          NULL,0,0},
  {NULL,0,0,NULL,NULL,0}};

/* list of drives that affect SCSI default settings */

static exception scsi_list[]={
  {"TOSHIBA",                     -1,0x82,scsi_enable_cdda,scsi_read_28,  0,0},
  {"IBM",                         -1,0x82,scsi_enable_cdda,scsi_read_28,  0,0},
  {"DEC",                         -1,0x82,scsi_enable_cdda,scsi_read_28,  0,0},
  
  {"IMS",                         -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"KODAK",                       -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"RICOH",                       -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"HP",                          -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"PHILIPS",                     -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"PLASMON",                     -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"GRUNDIG CDR100IPW",           -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},
  {"MITSUMI CD-R ",               -1,   0,scsi_enable_cdda,scsi_read_28,  1,0},

  {"YAMAHA",                      -1,   0,scsi_enable_cdda,        NULL,  0,0},

  {"PLEXTOR",                     -1,   0,            NULL,        NULL,  0,0},
  {"SONY",                        -1,   0,            NULL,        NULL,  0,0},

  {"NEC",                         -1,   0,            NULL,scsi_read_D4, 0,0},

  {NULL,0,0,NULL,NULL,0}};

