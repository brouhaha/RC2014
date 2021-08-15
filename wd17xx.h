struct wd17xx;

extern uint8_t wd17xx_read_data(struct wd17xx *fdc);
extern void wd17xx_write_data(struct wd17xx *fdc, uint8_t data);
extern uint8_t wd17xx_read_sector(struct wd17xx *fdc);
extern void wd17xx_write_sector(struct wd17xx *fdc, uint8_t data);
extern uint8_t wd17xx_read_track(struct wd17xx *fdc);
extern void wd17xx_write_track(struct wd17xx *fdc, uint8_t data);
extern void wd17xx_command(struct wd17xx *fdc, uint8_t val);
extern uint8_t wd17xx_status(struct wd17xx *fdc);
extern uint8_t wd17xx_status_noclear(struct wd17xx *fdc);
extern void wd17xx_set_drive(struct wd17xx *fdc, unsigned int drive);
extern struct wd17xx *wd17xx_create(void);
extern void wd17xx_free(struct wd17xx *fdc);
extern void wd17xx_detach(struct wd17xx *fdc, int dev);
extern int wd17xx_attach(struct wd17xx *fdc, int dev, const char *path, unsigned int sides, unsigned int tracks, unsigned int sectors, unsigned int secsize);
extern void wd17xx_trace(struct wd17xx *fdc, unsigned int onoff);
extern uint8_t wd17xx_intrq(struct wd17xx *fdc);
