/* e_file.h - whole-file load/save, chunk reader for .trk-style files (DESIGN 4.5), paths. Portable. Owner: T01.
   Chunk file header (16 bytes): tag[4], u32 version, u32 numChunks, u32 crc32 of everything after the header.
   Chunks: id[4], u32 size, data, padded to 4 bytes. chunk_open checks size, tag and CRC (logs err: lines).
   Paths: the base directory is "" (current directory) until file_set_base_dir (main.cpp: the exe folder).
   path_join and data_path use the native separator ('\\' on Windows, '/' elsewhere) and convert the other one,
   so "tracks/x.trk" works on both. An absolute rel ("C:\..", "\..", "/..") is used as is. */
#ifndef E_FILE_H
#define E_FILE_H

#include "e_base.h"

void *file_load(const char *path, int *size);      /* malloc'd, +1 zero byte; NULL on error */
int   file_save(const char *path, const void *p, int size);
typedef struct ChunkFile { const u8 *data; int size; u32 tag; u32 version; } ChunkFile;
int   chunk_open(ChunkFile *cf, const void *data, int size, u32 tag);  /* checks header (4.5) */
const u8 *chunk_find(const ChunkFile *cf, const char id[4], int *size); /* NULL if absent */
void  path_join(char *out, int n, const char *dir, const char *rel);
void  data_path(char *out, int n, const char *rel); /* exe dir (Win) or cwd (Mac) + "data\" + rel */

/* ---- additions (T01) ---- */
void  file_set_base_dir(const char *dir);          /* exe folder; "" = current directory */
const char *file_base_dir(void);
void  base_path(char *out, int n, const char *rel); /* base dir + rel (jet.ini, jet.log, shots) */
int   file_exists(const char *path);
int   chunk_count(const ChunkFile *cf);            /* numChunks from the header */
const u8 *chunk_at(const ChunkFile *cf, int i, char id[4], int *size);   /* i-th chunk in file order, NULL past the end */

#endif
