/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006 by Barry Wardell
 *
 * Based on Rockbox iriver bootloader by Linus Nielsen Feltzing
 * and the ipodlinux bootloader by Daniel Palffy and Bernard Leach
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "common.h"
#include "cpu.h"
#include "file.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "storage.h"
#include "adc.h"
#include "button.h"
#include "disk.h"
#include "crc32-mi4.h"
#include <string.h>
#include "power.h"
#include "version.h"
#if defined(SANSA_E200) || defined(PHILIPS_SA9200)
#include "i2c.h"
#include "backlight-target.h"
#endif
#include "usb.h"
#if defined(SANSA_E200) || defined(SANSA_C200) || defined(PHILIPS_SA9200)
#include "usb_drv.h"
#endif
#if defined(SAMSUNG_YH925)
/* this function (in lcd-yh925.c) resets the screen orientation for the OF
 * for use with dualbooting */
void lcd_reset(void);
#endif

/* Show the Rockbox logo - in show_logo.c */
extern void show_logo(void);

/* Button definitions */
#if CONFIG_KEYPAD == IRIVER_H10_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_LEFT

#elif CONFIG_KEYPAD == SANSA_E200_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_LEFT

#elif CONFIG_KEYPAD == SANSA_C200_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_LEFT

#elif CONFIG_KEYPAD == MROBE100_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_POWER

#elif CONFIG_KEYPAD == PHILIPS_SA9200_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_VOL_UP

#elif CONFIG_KEYPAD == PHILIPS_HDD1630_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_MENU

#elif CONFIG_KEYPAD == PHILIPS_HDD6330_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_VOL_UP

#elif CONFIG_KEYPAD == SAMSUNG_YH_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_LEFT

#elif CONFIG_KEYPAD == SANSA_FUZE_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_LEFT

#elif CONFIG_KEYPAD == PBELL_VIBE500_PAD
#define BOOTLOADER_BOOT_OF      BUTTON_OK

#endif

/* Maximum allowed firmware image size. 10MB is more than enough */
#define MAX_LOADSIZE (10*1024*1024)

/* A buffer to load the original firmware or Rockbox into */
unsigned char *loadbuffer = (unsigned char *)DRAM_START;

/* Locations and sizes in hidden partition on Sansa */
#if (CONFIG_STORAGE & STORAGE_SD)
#define PPMI_SECTOR_OFFSET  1024
#define PPMI_SECTORS        1
#define MI4_HEADER_SECTORS  1
#define NUM_PARTITIONS      2

#else
#define NUM_PARTITIONS      1

#endif

#define MI4_HEADER_SIZE     0x200

/* mi4 header structure */
struct mi4header_t {
    unsigned char magic[4];
    uint32_t version;
    uint32_t length;
    uint32_t crc32;
    uint32_t enctype;
    uint32_t mi4size;
    uint32_t plaintext;
    uint32_t dsa_key[10];
    uint32_t pad[109];
    unsigned char type[4];
    unsigned char model[4];
};

/* PPMI header structure */
struct ppmi_header_t {
    unsigned char magic[4];
    uint32_t length;
    uint32_t pad[126];
};

inline unsigned int le2int(unsigned char* buf)
{
   int32_t res = (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];

   return res;
}

inline void int2le(unsigned int val, unsigned char* addr)
{
    addr[0] = val & 0xFF;
    addr[1] = (val >> 8) & 0xff;
    addr[2] = (val >> 16) & 0xff;
    addr[3] = (val >> 24) & 0xff;
}

struct tea_key {
  const char * name;
  uint32_t     key[4];
};

#define NUM_KEYS (sizeof(tea_keytable)/sizeof(tea_keytable[0]))
struct tea_key tea_keytable[] = {
  { "default" ,          { 0x20d36cc0, 0x10e8c07d, 0xc0e7dcaa, 0x107eb080 } },
  { "sansa",             { 0xe494e96e, 0x3ee32966, 0x6f48512b, 0xa93fbb42 } },
  { "sansa_gh",          { 0xd7b10538, 0xc662945b, 0x1b3fce68, 0xf389c0e6 } },
  { "sansa_103",         { 0x1d29ddc0, 0x2579c2cd, 0xce339e1a, 0x75465dfe } },
  { "rhapsody",          { 0x7aa9c8dc, 0xbed0a82a, 0x16204cc7, 0x5904ef38 } },
  { "p610",              { 0x950e83dc, 0xec4907f9, 0x023734b9, 0x10cfb7c7 } },
  { "p640",              { 0x220c5f23, 0xd04df68e, 0x431b5e25, 0x4dcc1fa1 } },
  { "virgin",            { 0xe83c29a1, 0x04862973, 0xa9b3f0d4, 0x38be2a9c } },
  { "20gc_eng",          { 0x0240772c, 0x6f3329b5, 0x3ec9a6c5, 0xb0c9e493 } },
  { "20gc_fre",          { 0xbede8817, 0xb23bfe4f, 0x80aa682d, 0xd13f598c } },
  { "elio_p722",         { 0x6af3b9f8, 0x777483f5, 0xae8181cc, 0xfa6d8a84 } },
  { "c200",              { 0xbf2d06fa, 0xf0e23d59, 0x29738132, 0xe2d04ca7 } },
  { "c200_103",          { 0x2a7968de, 0x15127979, 0x142e60a7, 0xe49c1893 } },
  { "c200_106",          { 0xa913d139, 0xf842f398, 0x3e03f1a6, 0x060ee012 } },
  { "view",              { 0x70e19bda, 0x0c69ea7d, 0x2b8b1ad1, 0xe9767ced } },
  { "sa9200",            { 0x33ea0236, 0x9247bdc5, 0xdfaedf9f, 0xd67c9d30 } },
  { "hdd1630/hdd63x0",   { 0x04543ced, 0xcebfdbad, 0xf7477872, 0x0d12342e } },
  { "vibe500",           { 0xe3a66156, 0x77c6b67a, 0xe821dca5, 0xca8ca37c } },
};

/*

tea_decrypt() from http://en.wikipedia.org/wiki/Tiny_Encryption_Algorithm

"Following is an adaptation of the reference encryption and decryption
routines in C, released into the public domain by David Wheeler and
Roger Needham:"

*/

/* NOTE: The mi4 version of TEA uses a different initial value to sum compared
         to the reference implementation and the main loop is 8 iterations, not
         32.
*/

static void tea_decrypt(uint32_t* v0, uint32_t* v1, uint32_t* k) {
    uint32_t sum=0xF1BBCDC8, i;                    /* set up */
    uint32_t delta=0x9E3779B9;                     /* a key schedule constant */
    uint32_t k0=k[0], k1=k[1], k2=k[2], k3=k[3];   /* cache key */
    for(i=0; i<8; i++) {                               /* basic cycle start */
        *v1 -= ((*v0<<4) + k2) ^ (*v0 + sum) ^ ((*v0>>5) + k3);
        *v0 -= ((*v1<<4) + k0) ^ (*v1 + sum) ^ ((*v1>>5) + k1);
        sum -= delta;                                   /* end cycle */
    }
}

/* mi4 files are encrypted in 64-bit blocks (two little-endian 32-bit
   integers) and the key is incremented after each block
 */

static void tea_decrypt_buf(unsigned char* src, unsigned char* dest, size_t n, uint32_t * key)
{
    uint32_t v0, v1;
    unsigned int i;

    for (i = 0; i < (n / 8); i++) {
        v0 = le2int(src);
        v1 = le2int(src+4);

        tea_decrypt(&v0, &v1, key);

        int2le(v0, dest);
        int2le(v1, dest+4);

        src += 8;
        dest += 8;

        /* Now increment the key */
        key[0]++;
        if (key[0]==0) {
            key[1]++;
            if (key[1]==0) {
                key[2]++;
                if (key[2]==0) {
                    key[3]++;
                }
            }
        }
    }
}

static inline bool tea_test_key(unsigned char magic_enc[8], uint32_t * key, int unaligned)
{
    unsigned char magic_dec[8];
    tea_decrypt_buf(magic_enc, magic_dec, 8, key);

    return (le2int(&magic_dec[4*unaligned]) == 0xaa55aa55);
}

static int tea_find_key(struct mi4header_t *mi4header, int fd)
{
    unsigned int i;
    int rc;
    uint32_t key[4];
    uint32_t keyinc;
    unsigned char magic_enc[8];
    int key_found = -1;
    unsigned int magic_location = mi4header->length-4;
    int unaligned = 0;

    if ( (magic_location % 8) != 0 )
    {
        unaligned = 1;
        magic_location -= 4;
    }

    /* Load encrypted magic 0xaa55aa55 to check key */
    lseek(fd, MI4_HEADER_SIZE + magic_location, SEEK_SET);
    rc = read(fd, magic_enc, 8);
    if(rc < 8 )
        return EREAD_IMAGE_FAILED;

    printf("Searching for key:");

    for (i=0; i < NUM_KEYS && (key_found<0) ; i++) {
        key[0] = tea_keytable[i].key[0];
        key[1] = tea_keytable[i].key[1];
        key[2] = tea_keytable[i].key[2];
        key[3] = tea_keytable[i].key[3];

        /* Now increment the key */
        keyinc = (magic_location-mi4header->plaintext)/8;
        if ((key[0]+keyinc) < key[0]) key[1]++;
        key[0] += keyinc;
        if (key[1]==0) key[2]++;
        if (key[2]==0) key[3]++;

        if (tea_test_key(magic_enc,key,unaligned))
        {
            key_found = i;
             printf("%s...found", tea_keytable[i].name);
        } else {
           /* printf("%s...failed", tea_keytable[i].name); */
        }
    }

    return key_found;
}


/* Load mi4 format firmware image */
int load_mi4(unsigned char* buf, char* firmware, unsigned int buffer_size)
{
    int fd;
    struct mi4header_t mi4header;
    int rc;
    unsigned long sum;
    char filename[MAX_PATH];

    snprintf(filename,sizeof(filename), BOOTDIR "/%s",firmware);
    fd = open(filename, O_RDONLY);
    if(fd < 0)
    {
        snprintf(filename,sizeof(filename),"/%s",firmware);
        fd = open(filename, O_RDONLY);
        if(fd < 0)
            return EFILE_NOT_FOUND;
    }

    read(fd, &mi4header, MI4_HEADER_SIZE);

    /* MI4 file size */
    printf("mi4 size: %x", mi4header.mi4size);

    if ((mi4header.mi4size-MI4_HEADER_SIZE) > buffer_size)
    {
        close(fd);
        return EFILE_TOO_BIG;
    }

    /* CRC32 */
    printf("CRC32: %x", mi4header.crc32);

    /* Rockbox model id */
    printf("Model id: %.4s", mi4header.model);

    /* Read binary type (RBOS, RBBL) */
    printf("Binary type: %.4s", mi4header.type);

    /* Load firmware file */
    lseek(fd, MI4_HEADER_SIZE, SEEK_SET);
    rc = read(fd, buf, mi4header.mi4size-MI4_HEADER_SIZE);
    if(rc < (int)mi4header.mi4size-MI4_HEADER_SIZE)
    {
        close(fd);
        return EREAD_IMAGE_FAILED;
    }
 
    /* Check CRC32 to see if we have a valid file */
    sum = chksum_crc32 (buf, mi4header.mi4size - MI4_HEADER_SIZE);

    printf("Calculated CRC32: %x", sum);

    if(sum != mi4header.crc32)
    {
        close(fd);
        return EBAD_CHKSUM;
    }

    if( (mi4header.plaintext + MI4_HEADER_SIZE) != mi4header.mi4size)
    {
        /* Load encrypted firmware */
        int key_index = tea_find_key(&mi4header, fd);
        
        if (key_index < 0)
        {
            close(fd);
            return EINVALID_FORMAT;
        }
        
        /* Plaintext part is already loaded */
        buf += mi4header.plaintext;
        
        /* Decrypt in-place */
        tea_decrypt_buf(buf, buf, 
                        mi4header.mi4size-(mi4header.plaintext+MI4_HEADER_SIZE),
                        tea_keytable[key_index].key);

        printf("%s key used", tea_keytable[key_index].name);
        
        /* Check decryption was successfull */
        if(le2int(&buf[mi4header.length-mi4header.plaintext-4]) != 0xaa55aa55)
        {
            close(fd);
            return EREAD_IMAGE_FAILED;
        }
    }

    close(fd);
    return EOK;
}

#if (CONFIG_STORAGE & STORAGE_SD)
/* Load mi4 firmware from a hidden disk partition */
int load_mi4_part(unsigned char* buf, struct partinfo* pinfo,
                  unsigned int buffer_size, bool disable_rebuild)
{
    struct mi4header_t mi4header;
    struct ppmi_header_t ppmi_header;
    unsigned long sum;
    
    /* Read header to find out how long the mi4 file is. */
    storage_read_sectors(pinfo->start + PPMI_SECTOR_OFFSET,
                         PPMI_SECTORS, &ppmi_header);
    
    /* The first four characters at 0x80000 (sector 1024) should be PPMI*/
    if( memcmp(ppmi_header.magic, "PPMI", 4) )
        return EFILE_NOT_FOUND;
    
    printf("BL mi4 size: %x", ppmi_header.length);
    
    /* Read mi4 header of the OF */
    storage_read_sectors(pinfo->start + PPMI_SECTOR_OFFSET + PPMI_SECTORS 
                       + (ppmi_header.length/512), MI4_HEADER_SECTORS, &mi4header);
    
    /* We don't support encrypted mi4 files yet */
    if( (mi4header.plaintext) != (mi4header.mi4size-MI4_HEADER_SIZE))
        return EINVALID_FORMAT;

    /* MI4 file size */
    printf("OF mi4 size: %x", mi4header.mi4size);

    if ((mi4header.mi4size-MI4_HEADER_SIZE) > buffer_size)
        return EFILE_TOO_BIG;

    /* CRC32 */
    printf("CRC32: %x", mi4header.crc32);

    /* Rockbox model id */
    printf("Model id: %.4s", mi4header.model);

    /* Read binary type (RBOS, RBBL) */
    printf("Binary type: %.4s", mi4header.type);

    /* Load firmware */
    storage_read_sectors(pinfo->start + PPMI_SECTOR_OFFSET + PPMI_SECTORS
                        + (ppmi_header.length/512) + MI4_HEADER_SECTORS,
                        (mi4header.mi4size-MI4_HEADER_SIZE)/512, buf);

    /* Check CRC32 to see if we have a valid file */
    sum = chksum_crc32 (buf,mi4header.mi4size-MI4_HEADER_SIZE);

    printf("Calculated CRC32: %x", sum);

    if(sum != mi4header.crc32)
        return EBAD_CHKSUM;
    
#ifdef SANSA_E200    
    if (disable_rebuild)
    {
        char block[512];
        
        printf("Disabling database rebuild");
        
        storage_read_sectors(pinfo->start + 0x3c08, 1, block);
        block[0xe1] = 0;
        storage_write_sectors(pinfo->start + 0x3c08, 1, block);
    }
#else
    (void) disable_rebuild;
#endif

    return EOK;
}
#endif /* (CONFIG_STORAGE & STORAGE_SD) */

#ifdef HAVE_BOOTLOADER_USB_MODE
/* Return USB_HANDLED if session took place else return USB_EXTRACTED */
static int handle_usb(int connect_timeout)
{
    static struct event_queue q SHAREDBSS_ATTR;
    struct queue_event ev;
    int usb = USB_EXTRACTED;
    long end_tick = 0;
    
    if (!usb_plugged())
        return USB_EXTRACTED;

    queue_init(&q, true);
    usb_init();
    usb_start_monitoring();

    printf("USB: Connecting");

    if (connect_timeout != TIMEOUT_BLOCK)
        end_tick = current_tick + connect_timeout;

    while (1)
    {
        /* Sleep no longer than 1/2s */
        queue_wait_w_tmo(&q, &ev, HZ/2);

        if (ev.id == SYS_USB_CONNECTED)
        {
            /* Switch to verbose mode if not in it so that the status updates
             * are shown */
            verbose = true;
            /* Got the message - wait for disconnect */
            printf("Bootloader USB mode");

            usb = USB_HANDLED;
            usb_acknowledge(SYS_USB_CONNECTED_ACK);
            usb_wait_for_disconnect(&q);
            break;
        }

        if (connect_timeout != TIMEOUT_BLOCK &&
            TIME_AFTER(current_tick, end_tick))
        {
            /* Timed out waiting for the connect - will happen when connected
             * to a charger instead of a host port and the charging pin is
             * the same as the USB pin */
            printf("USB: Timed out");
            break;
        }

        if (!usb_plugged())
            break; /* Cable pulled */
    }

    usb_close();
    queue_delete(&q);

    return usb;
}
#elif (defined(SANSA_E200) || defined(SANSA_C200) || defined(PHILIPS_SA9200) \
    || defined (SANSA_VIEW)) && !defined(USE_ROCKBOX_USB)
/* Return USB_INSERTED if cable present */
static int handle_usb(int connect_timeout)
{
    int usb_retry = 0;
    int usb = USB_EXTRACTED;

    usb_init();
    while (usb_drv_powered() && usb_retry < 5 && usb != USB_INSERTED)
    {
        usb_retry++;
        sleep(HZ/4);
        usb = usb_detect();
    }

    if (usb != USB_INSERTED)
        usb = USB_EXTRACTED;

    return usb;
    (void)connect_timeout;
}
#else
/* Ignore cable state */
static int handle_usb(int connect_timeout)
{
    return USB_EXTRACTED;
    (void)connect_timeout;
}
#endif /* HAVE_BOOTLOADER_USB_MODE */

void* main(void)
{
    int i;
    int btn;
    int rc;
    int num_partitions;
    struct partinfo* pinfo;
#if !(CONFIG_STORAGE & STORAGE_SD)
    char buf[256];
    unsigned short* identify_info;
#endif
    int usb = USB_EXTRACTED;

    chksum_crc32gentab ();

    system_init();
    kernel_init();

#ifdef HAVE_BOOTLOADER_USB_MODE
    /* loader must service interrupts */
    enable_interrupt(IRQ_FIQ_STATUS);
#endif

    lcd_init();

    font_init();
    show_logo();

    adc_init();
#ifdef HAVE_BOOTLOADER_USB_MODE
    button_init_device();
#else
    button_init();
#endif
#if defined(SANSA_E200) || defined(PHILIPS_SA9200)
    i2c_init();
    _backlight_on();
#endif

    if (button_hold())
    {
        verbose = true;
        lcd_clear_display();
        printf("Hold switch on");
        printf("Shutting down...");
        sleep(HZ);
        power_off();
    }

    btn = button_read_device();

    /* Enable bootloader messages if any button is pressed */
#ifdef HAVE_BOOTLOADER_USB_MODE
    lcd_clear_display();
    if (btn)
        verbose = true;
#else
    if (btn) {
        lcd_clear_display();
        verbose = true;
    }
#endif

    lcd_setfont(FONT_SYSFIXED);

    printf("Rockbox boot loader");
    printf("Version: " RBVERSION);
    printf(MODEL_NAME);

    i=storage_init();
#if !(CONFIG_STORAGE & STORAGE_SD)
    if (i==0) {
        identify_info=ata_get_identify();
        /* Show model */
        for (i=0; i < 20; i++) {
            ((unsigned short*)buf)[i]=htobe16(identify_info[i+27]);
        }
        buf[40]=0;
        for (i=39; i && buf[i]==' '; i--) {
            buf[i]=0;
        }
        printf(buf);
    } else {
        error(EATA, i, true);
    }
#endif

    disk_init(IF_MV(0));
    num_partitions = disk_mount_all();
    if (num_partitions<=0)
    {
        error(EDISK,num_partitions, true);
    }

    /* Just list the first 2 partitions since we don't have any devices yet 
       that have more than that */
    for(i=0; i<NUM_PARTITIONS; i++)
    {
        pinfo = disk_partinfo(i);
        printf("Partition %d: 0x%02x %ld MB",
                i, pinfo->type, pinfo->size / 2048);
    }

    /* Now that storage is initialized, check for USB connection */
    if ((btn & BOOTLOADER_BOOT_OF) == 0)
    {
        usb_pin_init();
        usb = handle_usb(HZ*2);
        if (usb == USB_INSERTED)
            btn |= BOOTLOADER_BOOT_OF;
    }

    /* Try loading Rockbox, if that fails, fall back to the OF */
    if((btn & BOOTLOADER_BOOT_OF) == 0)
    {
        printf("Loading Rockbox...");
        rc = load_mi4(loadbuffer, BOOTFILE, MAX_LOADSIZE);
        if (rc < EOK)
        {
            bool old_verbose = verbose;
            verbose = true;
            printf("Can't load " BOOTFILE ": ");
            printf(strerror(rc));
            verbose = old_verbose;
            btn |= BOOTLOADER_BOOT_OF;
            sleep(5*HZ);
        }
        else
            goto main_exit;
    }

    if(btn & BOOTLOADER_BOOT_OF)
    {
        /* Load original mi4 firmware in to a memory buffer called loadbuffer.
           The rest of the loading is done in crt0.S.
           1) First try reading from the hidden partition (on Sansa only).
           2) Next try a decrypted mi4 file in /System/OF.mi4
           3) Finally, try a raw firmware binary in /System/OF.bin. It should be
              a mi4 firmware decrypted and header stripped using mi4code.
        */
        printf("Loading original firmware...");

#if (CONFIG_STORAGE & STORAGE_SD)
        /* First try a (hidden) firmware partition */
        printf("Trying firmware partition");
        pinfo = disk_partinfo(1);
        if(pinfo->type == PARTITION_TYPE_OS2_HIDDEN_C_DRIVE)
        {
            rc = load_mi4_part(loadbuffer, pinfo, MAX_LOADSIZE,
                               usb == USB_INSERTED);
            if (rc < EOK) {
                printf("Can't load from partition");
                printf(strerror(rc));
            } else {
                goto main_exit;
            }
        } else {
            printf("No hidden partition found.");
        }
#endif

#if defined(PHILIPS_HDD1630) || defined(PHILIPS_HDD6330) || defined(PHILIPS_SA9200)
        printf("Trying /System/OF.ebn");
        rc=load_mi4(loadbuffer, "/System/OF.ebn", MAX_LOADSIZE);
        if (rc < EOK) {
            printf("Can't load /System/OF.ebn");
            printf(strerror(rc));
        } else {
            goto main_exit;
        }
#endif

        printf("Trying /System/OF.mi4");
        rc=load_mi4(loadbuffer, "/System/OF.mi4", MAX_LOADSIZE);
        if (rc < EOK) {
            printf("Can't load /System/OF.mi4");
            printf(strerror(rc));
        } else {
#if defined(SAMSUNG_YH925)
            lcd_reset();
#endif
            goto main_exit;
        }

        printf("Trying /System/OF.bin");
        rc=load_raw_firmware(loadbuffer, "/System/OF.bin", MAX_LOADSIZE);
        if (rc < EOK) {
            printf("Can't load /System/OF.bin");
            printf(strerror(rc));
        } else {
#if defined(SAMSUNG_YH925)
            lcd_reset();
#endif
            goto main_exit;
        }
        
        error(0, 0, true);
    }

main_exit:
#ifdef HAVE_BOOTLOADER_USB_MODE
    storage_close();
    system_prepare_fw_start();
#endif

    return (void*)loadbuffer;
}
