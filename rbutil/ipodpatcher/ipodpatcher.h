/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2006-2007 Dave Chapman
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

#ifndef _IPODPATCHER_H
#define _IPODPATCHER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ipodio.h"

/* Size of buffer for disk I/O - 8MB is large enough for any version
   of the Apple firmware, but not the Nano's RSRC image. */
#define BUFFER_SIZE 8*1024*1024

#ifndef _MSC_VER
extern unsigned char* ipod_sectorbuf;
#else
/* MSVC needs to use dllimport to allow using it directly from a DLL.
 * See http://support.microsoft.com/kb/90530
 * Building with MSVC is only when using as DLL.
 */
_declspec(dllimport) unsigned char* ipod_sectorbuf;
#endif

extern int ipod_verbose;

#define FILETYPE_DOT_IPOD 0
#define FILETYPE_DOT_BIN  1
#ifdef WITH_BOOTOBJS
  #define FILETYPE_INTERNAL 2
#endif

char* get_parttype(int pt);
int read_partinfo(struct ipod_t* ipod, int silent);
int read_partition(struct ipod_t* ipod, int outfile);
int write_partition(struct ipod_t* ipod, int infile);
int diskmove(struct ipod_t* ipod, int delta);
int add_bootloader(struct ipod_t* ipod, char* filename, int type);
int delete_bootloader(struct ipod_t* ipod);
int write_firmware(struct ipod_t* ipod, char* filename, int type);
int read_firmware(struct ipod_t* ipod, char* filename, int type);
int read_directory(struct ipod_t* ipod);
int list_images(struct ipod_t* ipod);
int getmodel(struct ipod_t* ipod, int ipod_version);
int ipod_scan(struct ipod_t* ipod);
int write_dos_partition_table(struct ipod_t* ipod);
int ipod_get_xmlinfo(struct ipod_t* ipod);
void ipod_get_ramsize(struct ipod_t* ipod);
int read_aupd(struct ipod_t* ipod, char* filename);
int write_aupd(struct ipod_t* ipod, char* filename);
off_t filesize(int fd);
int ipod_has_bootloader(struct ipod_t* ipod);

#ifdef __cplusplus
}
#endif
#endif

