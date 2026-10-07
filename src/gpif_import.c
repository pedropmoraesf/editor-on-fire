/*
 * Editor On Fire - Guitar Pro GPIF importer
 *
 * Adds direct import of ZIP based Guitar Pro files containing
 * Content/score.gpif (GP7/GP8 family) without converting through GP5.
 *
 * Dependencies:
 *   libxml2 - GPIF XML parsing
 *   zlib    - raw DEFLATE stream used by ZIP entries
 *
 * The ZIP reader intentionally implements only what is required to safely
 * locate/extract Content/score.gpif.  It reads the central directory, accepts
 * stored (method 0) and deflated (method 8) entries, rejects encryption and
 * ZIP64, and limits the inflated GPIF size to protect against malformed files.
 */

#ifdef EOF_BUILD

#include <allegro.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <zlib.h>

#include "beat.h"
#include "gp_import.h"
#include "gpif_import.h"
#include "main.h"
#include "midi.h"
#include "note.h"
#include "rs.h"
#include "song.h"
#include "undo.h"
#include "utility.h"
#include "menu/edit.h"
#include "menu/track.h"

/* eof_apply_ts() is used by gp_import.c as well.  It is defined in EOF's beat
 * handling code, but is not declared in beat.h on all supported EOF revisions. */
extern int eof_apply_ts(unsigned num, unsigned den, unsigned long beat, EOF_SONG *sp, char undo);

#define GPIF_SCORE_PATH "Content/score.gpif"
#define GPIF_MAX_XML_SIZE (128UL * 1024UL * 1024UL)
#define GPIF_MAX_ZIP_ENTRIES 65535U
#define GPIF_MAX_NATIVE_STRINGS 16
#define GPIF_TFLAG_HOPO_ORIGIN 0x40000000UL
#define GPIF_TFLAG_STACCATO    0x20000000UL

/* ---------- small helpers ---------- */

static char *gpif_strdup(const char *src)
{
	char *ret;
	size_t len;

	if(!src)
		return NULL;
	len = strlen(src);
	ret = malloc(len + 1);
	if(ret)
		memcpy(ret, src, len + 1);
	return ret;
}

static int gpif_stricmp(const char *a, const char *b)
{
	unsigned char ca, cb;
	if(!a || !b)
		return (a != b);
	while(*a && *b)
	{
		ca = (unsigned char)tolower((unsigned char)*a++);
		cb = (unsigned char)tolower((unsigned char)*b++);
		if(ca != cb)
			return (int)ca - (int)cb;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static int gpif_contains_ci(const char *haystack, const char *needle)
{
	size_t nlen, i;
	if(!haystack || !needle)
		return 0;
	nlen = strlen(needle);
	if(!nlen)
		return 1;
	for(i = 0; haystack[i]; i++)
	{
		size_t j;
		for(j = 0; j < nlen && haystack[i + j]; j++)
		{
			if(tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
				break;
		}
		if(j == nlen)
			return 1;
	}
	return 0;
}

static uint16_t gpif_le16(const unsigned char *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t gpif_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void gpif_set_error(char *buffer, size_t size, const char *message)
{
	if(buffer && size)
	{
		(void)snprintf(buffer, size, "%s", message ? message : "Unknown GPIF error");
		buffer[size - 1] = '\0';
	}
}

/* ---------- minimal ZIP reader ---------- */

/*
 * IMPORTANT (Windows/EOF): Do not use the C runtime fopen() for the Guitar
 * Pro path here.  EOF's file chooser supplies UTF-8 paths and Allegro's
 * PACKFILE layer is what the rest of EOF uses to open them.  Narrow fopen()
 * on MinGW uses the Windows ANSI code page and fails for perfectly valid song
 * names containing characters such as á/é/ç.  When that happened the GPIF
 * detector returned false and eof_load_gp() fell through to the legacy GP5
 * reader, producing the misleading "GP5 format or older" error.
 *
 * Read the container through pack_fopen()/pack_fread() and do random access
 * on the in-memory byte buffer.  Besides fixing Unicode paths, this also
 * avoids relying on stdio seek semantics for Allegro-managed filenames.
 */

typedef struct
{
	uint16_t method;
	uint16_t flags;
	uint32_t crc32;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint32_t local_header_offset;
} GPIF_ZIP_ENTRY;

#define GPIF_ZIP_READ_CHUNK (64UL * 1024UL)
#define GPIF_MAX_CONTAINER_SIZE (512UL * 1024UL * 1024UL)

static int gpif_read_container(const char *fn, unsigned char **data, size_t *data_size, char *error, size_t error_size)
{
	PACKFILE *pf = NULL;
	unsigned char *buffer = NULL;
	size_t used = 0, capacity = GPIF_ZIP_READ_CHUNK;
	int ok = 0;

	if(data)
		*data = NULL;
	if(data_size)
		*data_size = 0;
	if(!fn || !data || !data_size)
		return 0;

	pf = pack_fopen(fn, "rb");
	if(!pf)
	{
		gpif_set_error(error, error_size, "Unable to open Guitar Pro file");
		return 0;
	}

	buffer = malloc(capacity);
	if(!buffer)
	{
		gpif_set_error(error, error_size, "Out of memory while reading Guitar Pro file");
		goto cleanup;
	}

	for(;;)
	{
		long got;
		size_t available;
		long request;

		if(used == capacity)
		{
			size_t new_capacity;
			unsigned char *resized;

			if(capacity >= GPIF_MAX_CONTAINER_SIZE)
			{
				gpif_set_error(error, error_size, "Guitar Pro container exceeds the 512 MiB safety limit");
				goto cleanup;
			}
			new_capacity = capacity * 2U;
			if((new_capacity < capacity) || (new_capacity > GPIF_MAX_CONTAINER_SIZE))
				new_capacity = GPIF_MAX_CONTAINER_SIZE;
			resized = realloc(buffer, new_capacity);
			if(!resized)
			{
				gpif_set_error(error, error_size, "Out of memory while reading Guitar Pro file");
				goto cleanup;
			}
			buffer = resized;
			capacity = new_capacity;
		}

		available = capacity - used;
		if(available > GPIF_ZIP_READ_CHUNK)
			available = GPIF_ZIP_READ_CHUNK;
		request = (long)available;
		got = pack_fread(buffer + used, request, pf);
		if(got <= 0)
			break;
		used += (size_t)got;
		if(got < request)
			break;
	}

	if(used < 4U)
	{
		gpif_set_error(error, error_size, "Guitar Pro file is empty or truncated");
		goto cleanup;
	}

	*data = buffer;
	*data_size = used;
	buffer = NULL;
	ok = 1;

cleanup:
	free(buffer);
	if(pf)
		pack_fclose(pf);
	return ok;
}

int eof_gpif_is_container(const char *fn)
{
	PACKFILE *pf;
	unsigned char sig[4];
	int result = 0;

	if(!fn)
		return 0;

	/* Use Allegro's UTF-8 aware file layer, just like the legacy GP importer. */
	pf = pack_fopen(fn, "rb");
	if(!pf)
		return 0;
	if(pack_fread(sig, (long)sizeof(sig), pf) == (long)sizeof(sig))
	{
		if((sig[0] == 'P') && (sig[1] == 'K') &&
		   (((sig[2] == 3) && (sig[3] == 4)) ||
		    ((sig[2] == 5) && (sig[3] == 6)) ||
		    ((sig[2] == 7) && (sig[3] == 8))))
			result = 1;
	}
	pack_fclose(pf);
	return result;
}

static int gpif_zip_name_equals(const unsigned char *name, size_t length, const char *wanted)
{
	size_t i, wanted_len;

	if(!name || !wanted)
		return 0;
	wanted_len = strlen(wanted);
	if(length != wanted_len)
		return 0;
	for(i = 0; i < length; i++)
	{
		if(tolower((unsigned char)name[i]) != tolower((unsigned char)wanted[i]))
			return 0;
	}
	return 1;
}

static int gpif_zip_find_score(const unsigned char *zip, size_t zip_size, GPIF_ZIP_ENTRY *entry, char *error, size_t error_size)
{
	size_t scan_start, pos, eocd_pos = (size_t)-1;
	const unsigned char *eocd;
	uint16_t disk_no, cd_disk, entries_on_disk, entries;
	uint32_t cd_size, cd_offset;
	unsigned int index;

	if(!zip || !entry || (zip_size < 22U))
	{
		gpif_set_error(error, error_size, "Invalid or truncated ZIP container");
		return 0;
	}

	/* EOCD can be followed by at most 65535 bytes of comment. */
	scan_start = (zip_size > (size_t)(22U + 65535U)) ? zip_size - (size_t)(22U + 65535U) : 0U;
	pos = zip_size - 22U;
	for(;;)
	{
		if((zip[pos] == 0x50) && (zip[pos + 1] == 0x4B) && (zip[pos + 2] == 0x05) && (zip[pos + 3] == 0x06))
		{
			eocd_pos = pos;
			break;
		}
		if(pos == scan_start)
			break;
		pos--;
	}
	if(eocd_pos == (size_t)-1)
	{
		gpif_set_error(error, error_size, "ZIP end-of-central-directory record not found");
		return 0;
	}

	eocd = zip + eocd_pos;
	disk_no = gpif_le16(eocd + 4);
	cd_disk = gpif_le16(eocd + 6);
	entries_on_disk = gpif_le16(eocd + 8);
	entries = gpif_le16(eocd + 10);
	cd_size = gpif_le32(eocd + 12);
	cd_offset = gpif_le32(eocd + 16);

	if(disk_no || cd_disk || (entries_on_disk != entries))
	{
		gpif_set_error(error, error_size, "Multi-disk ZIP Guitar Pro containers are not supported");
		return 0;
	}
	if((entries == 0xFFFFU) || (cd_size == 0xFFFFFFFFUL) || (cd_offset == 0xFFFFFFFFUL))
	{
		gpif_set_error(error, error_size, "ZIP64 Guitar Pro containers are not supported");
		return 0;
	}
	if(entries > GPIF_MAX_ZIP_ENTRIES)
	{
		gpif_set_error(error, error_size, "ZIP has too many entries");
		return 0;
	}
	if(((uint64_t)cd_offset + (uint64_t)cd_size > (uint64_t)zip_size) || ((size_t)cd_offset >= zip_size))
	{
		gpif_set_error(error, error_size, "Invalid ZIP central-directory range");
		return 0;
	}

	pos = (size_t)cd_offset;
	for(index = 0; index < entries; index++)
	{
		const unsigned char *hdr;
		uint16_t fnlen, extralen, commentlen;
		size_t record_size;

		if((pos > zip_size) || (zip_size - pos < 46U))
		{
			gpif_set_error(error, error_size, "Truncated ZIP central directory");
			return 0;
		}
		hdr = zip + pos;
		if(gpif_le32(hdr) != 0x02014B50UL)
		{
			gpif_set_error(error, error_size, "Malformed ZIP central directory");
			return 0;
		}
		fnlen = gpif_le16(hdr + 28);
		extralen = gpif_le16(hdr + 30);
		commentlen = gpif_le16(hdr + 32);
		record_size = 46U + (size_t)fnlen + (size_t)extralen + (size_t)commentlen;
		if(record_size > zip_size - pos)
		{
			gpif_set_error(error, error_size, "Truncated ZIP central-directory entry");
			return 0;
		}

		if(gpif_zip_name_equals(hdr + 46, fnlen, GPIF_SCORE_PATH))
		{
			entry->flags = gpif_le16(hdr + 8);
			entry->method = gpif_le16(hdr + 10);
			entry->crc32 = gpif_le32(hdr + 16);
			entry->compressed_size = gpif_le32(hdr + 20);
			entry->uncompressed_size = gpif_le32(hdr + 24);
			entry->local_header_offset = gpif_le32(hdr + 42);
			return 1;
		}
		pos += record_size;
	}

	gpif_set_error(error, error_size, "Content/score.gpif was not found in the Guitar Pro container");
	return 0;
}

static int gpif_zip_extract_score(const char *fn, unsigned char **out, size_t *out_size, char *error, size_t error_size)
{
	unsigned char *zip = NULL;
	size_t zip_size = 0;
	GPIF_ZIP_ENTRY entry;
	const unsigned char *local, *compressed;
	unsigned char *plain = NULL;
	uint16_t fnlen, extralen;
	size_t data_offset;
	int ok = 0;

	if(out)
		*out = NULL;
	if(out_size)
		*out_size = 0;
	if(!fn || !out || !out_size)
		return 0;

	if(!gpif_read_container(fn, &zip, &zip_size, error, error_size))
		return 0;

	memset(&entry, 0, sizeof(entry));
	if(!gpif_zip_find_score(zip, zip_size, &entry, error, error_size))
		goto cleanup;
	if(entry.flags & 1U)
	{
		gpif_set_error(error, error_size, "Encrypted GPIF ZIP entries are not supported");
		goto cleanup;
	}
	if((entry.method != 0U) && (entry.method != 8U))
	{
		gpif_set_error(error, error_size, "GPIF uses an unsupported ZIP compression method");
		goto cleanup;
	}
	if(!entry.uncompressed_size || (entry.uncompressed_size > GPIF_MAX_XML_SIZE))
	{
		gpif_set_error(error, error_size, "GPIF XML is empty or exceeds the 128 MiB safety limit");
		goto cleanup;
	}
	if((entry.compressed_size > GPIF_MAX_XML_SIZE) && (entry.method != 0U))
	{
		gpif_set_error(error, error_size, "Compressed GPIF entry exceeds the safety limit");
		goto cleanup;
	}

	if(((size_t)entry.local_header_offset > zip_size) || (zip_size - (size_t)entry.local_header_offset < 30U))
	{
		gpif_set_error(error, error_size, "Invalid GPIF local ZIP header offset");
		goto cleanup;
	}
	local = zip + (size_t)entry.local_header_offset;
	if(gpif_le32(local) != 0x04034B50UL)
	{
		gpif_set_error(error, error_size, "Invalid GPIF local ZIP header");
		goto cleanup;
	}
	fnlen = gpif_le16(local + 26);
	extralen = gpif_le16(local + 28);
	data_offset = (size_t)entry.local_header_offset + 30U + (size_t)fnlen + (size_t)extralen;
	if((data_offset > zip_size) || ((size_t)entry.compressed_size > zip_size - data_offset))
	{
		gpif_set_error(error, error_size, "Invalid or truncated GPIF ZIP payload");
		goto cleanup;
	}
	compressed = zip + data_offset;

	plain = malloc((size_t)entry.uncompressed_size + 1U);
	if(!plain)
	{
		gpif_set_error(error, error_size, "Out of memory while extracting GPIF XML");
		goto cleanup;
	}

	if(entry.method == 0U)
	{
		if(entry.compressed_size != entry.uncompressed_size)
		{
			gpif_set_error(error, error_size, "Invalid stored GPIF ZIP entry sizes");
			goto cleanup;
		}
		memcpy(plain, compressed, entry.uncompressed_size);
	}
	else
	{
		z_stream zs;
		int zret;
		memset(&zs, 0, sizeof(zs));
		zs.next_in = (Bytef *)compressed;
		zs.avail_in = entry.compressed_size;
		zs.next_out = plain;
		zs.avail_out = entry.uncompressed_size;
		if(inflateInit2(&zs, -MAX_WBITS) != Z_OK)
		{
			gpif_set_error(error, error_size, "Unable to initialize zlib for GPIF");
			goto cleanup;
		}
		zret = inflate(&zs, Z_FINISH);
		inflateEnd(&zs);
		if((zret != Z_STREAM_END) || (zs.total_out != entry.uncompressed_size))
		{
			gpif_set_error(error, error_size, "Unable to inflate GPIF XML");
			goto cleanup;
		}
	}

	if((uint32_t)crc32(0L, plain, entry.uncompressed_size) != entry.crc32)
	{
		gpif_set_error(error, error_size, "GPIF ZIP entry failed CRC-32 validation");
		goto cleanup;
	}
	plain[entry.uncompressed_size] = '\0';
	*out = plain;
	*out_size = entry.uncompressed_size;
	plain = NULL;
	ok = 1;

cleanup:
	free(plain);
	free(zip);
	return ok;
}

/* ---------- XML indexing ---------- */

typedef struct
{
	char *key;
	xmlNodePtr node;
} GPIF_MAP_SLOT;

typedef struct
{
	GPIF_MAP_SLOT *slot;
	size_t capacity;
	size_t count;
} GPIF_MAP;

typedef struct
{
	xmlNodePtr *item;
	size_t count;
	size_t capacity;
} GPIF_NODE_VECTOR;

typedef struct
{
	GPIF_MAP rhythms, notes, beats, voices, bars, texts, chords;
	GPIF_NODE_VECTOR tracks, master_bars;
} GPIF_INDEX;

static uint32_t gpif_hash(const char *s)
{
	uint32_t h = 2166136261U;
	while(s && *s)
	{
		h ^= (unsigned char)*s++;
		h *= 16777619U;
	}
	return h;
}

static void gpif_map_destroy(GPIF_MAP *map)
{
	size_t i;
	if(!map)
		return;
	for(i = 0; i < map->capacity; i++)
		free(map->slot[i].key);
	free(map->slot);
	memset(map, 0, sizeof(*map));
}

static int gpif_map_resize(GPIF_MAP *map, size_t new_capacity)
{
	GPIF_MAP_SLOT *old = map->slot;
	size_t oldcap = map->capacity, i;

	map->slot = calloc(new_capacity, sizeof(*map->slot));
	if(!map->slot)
	{
		map->slot = old;
		return 0;
	}
	map->capacity = new_capacity;
	map->count = 0;
	for(i = 0; i < oldcap; i++)
	{
		if(old[i].key)
		{
			size_t pos = gpif_hash(old[i].key) & (new_capacity - 1U);
			while(map->slot[pos].key)
				pos = (pos + 1U) & (new_capacity - 1U);
			map->slot[pos] = old[i];
			map->count++;
		}
	}
	free(old);
	return 1;
}

static int gpif_map_put(GPIF_MAP *map, const char *key, xmlNodePtr node)
{
	size_t pos;
	char *copy;
	if(!map || !key || !*key)
		return 1;
	if(!map->capacity)
	{
		if(!gpif_map_resize(map, 64U))
			return 0;
	}
	if((map->count + 1U) * 10U >= map->capacity * 7U)
	{
		if(!gpif_map_resize(map, map->capacity * 2U))
			return 0;
	}
	pos = gpif_hash(key) & (map->capacity - 1U);
	while(map->slot[pos].key)
	{
		if(!strcmp(map->slot[pos].key, key))
		{
			map->slot[pos].node = node;
			return 1;
		}
		pos = (pos + 1U) & (map->capacity - 1U);
	}
	copy = gpif_strdup(key);
	if(!copy)
		return 0;
	map->slot[pos].key = copy;
	map->slot[pos].node = node;
	map->count++;
	return 1;
}

static xmlNodePtr gpif_map_get(const GPIF_MAP *map, const char *key)
{
	size_t pos, start;
	if(!map || !map->capacity || !key)
		return NULL;
	pos = start = gpif_hash(key) & (map->capacity - 1U);
	do
	{
		if(!map->slot[pos].key)
			return NULL;
		if(!strcmp(map->slot[pos].key, key))
			return map->slot[pos].node;
		pos = (pos + 1U) & (map->capacity - 1U);
	} while(pos != start);
	return NULL;
}

static int gpif_vector_push(GPIF_NODE_VECTOR *vec, xmlNodePtr node)
{
	xmlNodePtr *tmp;
	size_t newcap;
	if(vec->count >= vec->capacity)
	{
		newcap = vec->capacity ? vec->capacity * 2U : 16U;
		tmp = realloc(vec->item, newcap * sizeof(*tmp));
		if(!tmp)
			return 0;
		vec->item = tmp;
		vec->capacity = newcap;
	}
	vec->item[vec->count++] = node;
	return 1;
}

static int gpif_node_is(xmlNodePtr node, const char *name)
{
	return node && (node->type == XML_ELEMENT_NODE) && !xmlStrcasecmp(node->name, BAD_CAST name);
}

static xmlNodePtr gpif_find_first(xmlNodePtr node, const char *name)
{
	xmlNodePtr cur, ret;
	if(!node)
		return NULL;
	for(cur = node->children; cur; cur = cur->next)
	{
		if(gpif_node_is(cur, name))
			return cur;
		ret = gpif_find_first(cur, name);
		if(ret)
			return ret;
	}
	return NULL;
}

static char *gpif_node_text(xmlNodePtr node)
{
	xmlChar *xtext;
	char *ret;
	if(!node)
		return NULL;
	xtext = xmlNodeGetContent(node);
	if(!xtext)
		return NULL;
	ret = gpif_strdup((const char *)xtext);
	xmlFree(xtext);
	return ret;
}

static char *gpif_get_text(xmlNodePtr node, const char *tag)
{
	return gpif_node_text(gpif_find_first(node, tag));
}

static int gpif_get_int(xmlNodePtr node, const char *tag, int fallback)
{
	char *text = gpif_get_text(node, tag), *end = NULL;
	long value;
	int parsed = 0;
	if(!text)
		return fallback;
	value = strtol(text, &end, 10);
	if(end && (end != text))
		parsed = 1;
	free(text);
	return parsed ? (int)value : fallback;
}

static int gpif_index_walk(xmlNodePtr node, GPIF_INDEX *idx)
{
	xmlNodePtr cur;
	for(cur = node; cur; cur = cur->next)
	{
		GPIF_MAP *map = NULL;
		xmlChar *id = NULL;
		if(cur->type == XML_ELEMENT_NODE)
		{
			if(gpif_node_is(cur, "Rhythm")) map = &idx->rhythms;
			else if(gpif_node_is(cur, "Note")) map = &idx->notes;
			else if(gpif_node_is(cur, "Beat")) map = &idx->beats;
			else if(gpif_node_is(cur, "Voice")) map = &idx->voices;
			else if(gpif_node_is(cur, "Bar")) map = &idx->bars;
			else if(gpif_node_is(cur, "Text")) map = &idx->texts;
			else if(gpif_node_is(cur, "Chord")) map = &idx->chords;
			else if(gpif_node_is(cur, "Track"))
			{
				if(!gpif_vector_push(&idx->tracks, cur))
					return 0;
			}
			else if(gpif_node_is(cur, "MasterBar"))
			{
				if(!gpif_vector_push(&idx->master_bars, cur))
					return 0;
			}

			if(map)
			{
				id = xmlGetProp(cur, BAD_CAST "id");
				if(id)
				{
					if(!gpif_map_put(map, (const char *)id, cur))
					{
						xmlFree(id);
						return 0;
					}
					xmlFree(id);
				}
			}
		}
		if(cur->children && !gpif_index_walk(cur->children, idx))
			return 0;
	}
	return 1;
}

static void gpif_index_destroy(GPIF_INDEX *idx)
{
	gpif_map_destroy(&idx->rhythms);
	gpif_map_destroy(&idx->notes);
	gpif_map_destroy(&idx->beats);
	gpif_map_destroy(&idx->voices);
	gpif_map_destroy(&idx->bars);
	gpif_map_destroy(&idx->texts);
	gpif_map_destroy(&idx->chords);
	free(idx->tracks.item);
	free(idx->master_bars.item);
	memset(idx, 0, sizeof(*idx));
}

/* ---------- XML property/list helpers ---------- */

static xmlNodePtr gpif_find_property(xmlNodePtr node, const char *property_name)
{
	xmlNodePtr cur;
	if(!node)
		return NULL;
	for(cur = node->children; cur; cur = cur->next)
	{
		if(gpif_node_is(cur, "Property"))
		{
			xmlChar *name = xmlGetProp(cur, BAD_CAST "name");
			if(name)
			{
				int match = !xmlStrcasecmp(name, BAD_CAST property_name);
				xmlFree(name);
				if(match)
					return cur;
			}
		}
		{
			xmlNodePtr found = gpif_find_property(cur, property_name);
			if(found)
				return found;
		}
	}
	return NULL;
}

static int gpif_property_enabled(xmlNodePtr node, const char *property_name)
{
	xmlNodePtr prop = gpif_find_property(node, property_name);
	return prop && (gpif_find_first(prop, "Enable") != NULL);
}

static int gpif_property_int(xmlNodePtr node, const char *property_name, int *out)
{
	xmlNodePtr prop = gpif_find_property(node, property_name), child;
	char *text, *end;
	long value;
	if(!prop)
	{
		/* Some GPIF revisions use direct elements instead of Property. */
		xmlNodePtr direct = gpif_find_first(node, property_name);
		if(!direct)
			return 0;
		text = gpif_node_text(direct);
	}
	else
	{
		text = NULL;
		for(child = prop->children; child; child = child->next)
		{
			if(child->type != XML_ELEMENT_NODE)
				continue;
			text = gpif_node_text(child);
			if(text && *text)
				break;
			free(text);
			text = NULL;
		}
		if(!text)
			text = gpif_node_text(prop);
	}
	if(!text)
		return 0;
	end = NULL;
	value = strtol(text, &end, 10);
	if(end == text)
	{
		free(text);
		return 0;
	}
	if(out)
		*out = (int)value;
	free(text);
	return 1;
}

static char *gpif_property_text(xmlNodePtr node, const char *property_name)
{
	xmlNodePtr prop = gpif_find_property(node, property_name), child;
	if(!prop)
		return NULL;
	for(child = prop->children; child; child = child->next)
	{
		char *text;
		if(child->type != XML_ELEMENT_NODE)
			continue;
		text = gpif_node_text(child);
		if(text && *text)
			return text;
		free(text);
	}
	return NULL;
}

typedef struct
{
	char **item;
	size_t count;
} GPIF_STRING_LIST;

static void gpif_string_list_destroy(GPIF_STRING_LIST *list)
{
	size_t i;
	if(!list)
		return;
	for(i = 0; i < list->count; i++)
		free(list->item[i]);
	free(list->item);
	memset(list, 0, sizeof(*list));
}

static int gpif_string_list_add(GPIF_STRING_LIST *list, const char *text)
{
	char **tmp;
	char *copy;
	if(!text || !*text)
		return 1;
	tmp = realloc(list->item, (list->count + 1U) * sizeof(*tmp));
	if(!tmp)
		return 0;
	list->item = tmp;
	copy = gpif_strdup(text);
	if(!copy)
		return 0;
	list->item[list->count++] = copy;
	return 1;
}

static GPIF_STRING_LIST gpif_id_list(xmlNodePtr container, const char *child_tag)
{
	GPIF_STRING_LIST result;
	xmlNodePtr child;
	int got_children = 0;
	memset(&result, 0, sizeof(result));
	if(!container)
		return result;

	for(child = container->children; child; child = child->next)
	{
		char *text = NULL;
		xmlChar *ref = NULL;
		if(child->type != XML_ELEMENT_NODE)
			continue;
		if(child_tag && !gpif_node_is(child, child_tag))
			continue;
		got_children = 1;
		ref = xmlGetProp(child, BAD_CAST "ref");
		if(!ref)
			ref = xmlGetProp(child, BAD_CAST "id");
		if(ref)
		{
			gpif_string_list_add(&result, (const char *)ref);
			xmlFree(ref);
			continue;
		}
		text = gpif_node_text(child);
		if(text)
		{
			gpif_string_list_add(&result, text);
			free(text);
		}
	}
	if(!got_children)
	{
		char *text = gpif_node_text(container), *p;
		if(text)
		{
			p = text;
			while(*p)
			{
				char *start;
				while(*p && isspace((unsigned char)*p)) p++;
				if(!*p) break;
				start = p;
				while(*p && !isspace((unsigned char)*p)) p++;
				if(*p) *p++ = '\0';
				gpif_string_list_add(&result, start);
			}
			free(text);
		}
	}
	return result;
}

/* ---------- EOF intermediate-structure helpers ---------- */

typedef struct
{
	EOF_PRO_GUITAR_TRACK *tp;
	int native_strings;
	int string_map[GPIF_MAX_NATIVE_STRINGS];
	int tuning_midi[GPIF_MAX_NATIVE_STRINGS];
	EOF_PRO_GUITAR_NOTE *previous[2][6];
	unsigned char previous_fret[2][6];
	unsigned char previous_valid[2][6];
} GPIF_TRACK_CONTEXT;

static void gpif_free_gp(struct eof_guitar_pro_struct *gp)
{
	unsigned long i, j;
	if(!gp)
		return;
	if(gp->track)
	{
		for(i = 0; i < gp->numtracks; i++)
		{
			if(gp->track[i])
			{
				for(j = 0; j < gp->track[i]->pgnotes; j++) free(gp->track[i]->pgnote[j]);
				for(j = 0; j < gp->track[i]->technotes; j++) free(gp->track[i]->technote[j]);
				free(gp->track[i]);
			}
		}
	}
	if(gp->names)
	{
		for(i = 0; i < gp->numtracks; i++) free(gp->names[i]);
	}
	for(i = 0; i < gp->text_events; i++) free(gp->text_event[i]);
	free(gp->track);
	free(gp->names);
	free(gp->instrument_types);
	free(gp->measure);
	free(gp);
}

static int gpif_add_text_event(struct eof_guitar_pro_struct *gp, const char *text, unsigned long measure)
{
	EOF_TEXT_EVENT *event;
	if(!gp || !text || !*text || gp->text_events >= EOF_MAX_TEXT_EVENTS)
		return 1;
	event = calloc(1, sizeof(*event));
	if(!event)
		return 0;
	(void)snprintf(event->text, sizeof(event->text), "%s", text);
	event->pos = measure; /* converted to beat number before note parsing */
	event->track = 0;
	event->flags = EOF_EVENT_FLAG_RS_PHRASE;
	event->is_temporary = 0;
	gp->text_event[gp->text_events++] = event;
	return 1;
}

static int gpif_instrument_from_name(const char *name)
{
	if(!name) return 0;
	if(gpif_contains_ci(name, "drum") || gpif_contains_ci(name, "bateria") || gpif_contains_ci(name, "perc")) return 3;
	if(gpif_contains_ci(name, "bass") || gpif_contains_ci(name, "baixo")) return 2;
	if(gpif_contains_ci(name, "guitar") || gpif_contains_ci(name, "gtr") || gpif_contains_ci(name, "lead") ||
	   gpif_contains_ci(name, "rhythm") || gpif_contains_ci(name, "solo") || gpif_contains_ci(name, "acoustic") ||
	   gpif_contains_ci(name, "clean") || gpif_contains_ci(name, "dist")) return 1;
	return 0;
}

static unsigned char gpif_arrangement_from_name(const char *name, int instrument)
{
	if(instrument == 2) return EOF_BASS_ARRANGEMENT;
	if(name && (gpif_contains_ci(name, "lead") || gpif_contains_ci(name, "solo"))) return EOF_LEAD_ARRANGEMENT;
	if(name && (gpif_contains_ci(name, "rhythm") || gpif_contains_ci(name, "base"))) return EOF_RHYTHM_ARRANGEMENT;
	return instrument == 1 ? EOF_COMBO_ARRANGEMENT : 0;
}

static int gpif_collect_pitches(xmlNodePtr track_node, int *pitches, int max_count)
{
	xmlNodePtr pnode = gpif_find_first(track_node, "Pitches"), child;
	int count = 0;
	if(!pnode)
		return 0;
	for(child = pnode->children; child && count < max_count; child = child->next)
	{
		char *text, *end;
		long value;
		if(child->type != XML_ELEMENT_NODE) continue;
		text = gpif_node_text(child);
		if(!text) continue;
		value = strtol(text, &end, 10);
		if(end != text) pitches[count++] = (int)value;
		free(text);
	}
	if(!count)
	{
		char *text = gpif_node_text(pnode), *p;
		if(!text) return 0;
		p = text;
		while(*p && count < max_count)
		{
			char *end;
			long value;
			while(*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
			if(!*p) break;
			value = strtol(p, &end, 10);
			if(end == p) { p++; continue; }
			pitches[count++] = (int)value;
			p = end;
		}
		free(text);
	}
	return count;
}

static int gpif_prepare_track(struct eof_guitar_pro_struct *gp, GPIF_TRACK_CONTEXT *ctx,
                              xmlNodePtr track_node, unsigned long index, int *wide_choice)
{
	EOF_PRO_GUITAR_TRACK *tp;
	char *name;
	int instrument, program = -1, fretcount = 24, capo = 0;
	int pitches[GPIF_MAX_NATIVE_STRINGS], count, first_kept = 0, i;

	name = gpif_get_text(track_node, "Name");
	if(!name || !*name)
	{
		char fallback[32];
		free(name);
		(void)snprintf(fallback, sizeof(fallback), "Track #%lu", index + 1UL);
		name = gpif_strdup(fallback);
	}
	if(!name)
		return 0;
	gp->names[index] = name;

	tp = calloc(1, sizeof(*tp));
	if(!tp)
		return 0;
	tp->note = tp->pgnote;
	tp->numfrets = 24;
	gp->track[index] = tp;
	ctx->tp = tp;
	for(i = 0; i < GPIF_MAX_NATIVE_STRINGS; i++) ctx->string_map[i] = -1;

	instrument = gpif_instrument_from_name(name);
	program = gpif_get_int(track_node, "Program", -1);
	if(program >= 24 && program <= 31) instrument = 1;
	else if(program >= 32 && program <= 39) instrument = 2;
	gp->instrument_types[index] = (char)instrument;
	tp->arrangement = gpif_arrangement_from_name(name, instrument);

	count = gpif_collect_pitches(track_node, pitches, GPIF_MAX_NATIVE_STRINGS);
	if(count <= 0)
	{
		static const int standard[] = {40, 45, 50, 55, 59, 64}; /* GPIF order: low -> high */
		count = 6;
		memcpy(pitches, standard, sizeof(standard));
	}
	ctx->native_strings = count;
	for(i = 0; i < count; i++) ctx->tuning_midi[i] = pitches[i];

	if(count > 6)
	{
		if(*wide_choice < 0)
		{
			eof_clear_input();
			*wide_choice = (alert("Warning: GP7/8 track has more than 6 strings.",
			                      "EOF can import six strings into a pro guitar track.",
			                      "Which side of the tuning should be preserved?",
			                      "&Six highest", "&Six lowest", 'h', 'l') == 2) ? 1 : 0;
		}
		if(*wide_choice == 0)
			first_kept = count - 6; /* normal 7-string -> discard the extra lowest string */
		else
			first_kept = 0;
		tp->numstrings = 6;
	}
	else
	{
		first_kept = 0;
		tp->numstrings = (unsigned char)count;
	}
	for(i = 0; i < count; i++)
	{
		if((i >= first_kept) && (i < first_kept + tp->numstrings))
		{
			int eof_string = i - first_kept;
			ctx->string_map[i] = eof_string;
			tp->tuning[eof_string] = (char)pitches[i]; /* intermediate GP struct stores absolute MIDI */
		}
	}

	if(gpif_property_int(track_node, "FretCount", &fretcount) || gpif_property_int(track_node, "Frets", &fretcount))
	{
		if(fretcount > 0 && fretcount < 128) tp->numfrets = (unsigned char)fretcount;
	}
	if(gpif_property_int(track_node, "CapoFret", &capo) || gpif_property_int(track_node, "Capo", &capo))
	{
		if(capo >= 0 && capo < 128) tp->capo = (unsigned char)capo;
	}
	return 1;
}

static double gpif_rhythm_length_beats(xmlNodePtr beat, const GPIF_INDEX *idx, unsigned denominator)
{
	xmlNodePtr rref = gpif_find_first(beat, "Rhythm"), rhythm = NULL, nv, dot, tuplet;
	xmlChar *ref = NULL;
	char *text = NULL;
	double whole = 0.25, result;
	int dot_count = 0, tnum = 1, tden = 1;

	if(rref)
	{
		ref = xmlGetProp(rref, BAD_CAST "ref");
		if(ref)
		{
			rhythm = gpif_map_get(&idx->rhythms, (const char *)ref);
			xmlFree(ref);
		}
		if(!rhythm) rhythm = rref;
	}
	if(!rhythm)
		return (double)denominator / 4.0;

	nv = gpif_find_first(rhythm, "NoteValue");
	text = gpif_node_text(nv);
	if(text)
	{
		if(gpif_contains_ci(text, "whole") || !strcmp(text, "1") || !strcmp(text, "1/1")) whole = 1.0;
		else if(gpif_contains_ci(text, "half") || !strcmp(text, "2") || !strcmp(text, "1/2")) whole = 0.5;
		else if(gpif_contains_ci(text, "quarter") || !strcmp(text, "4") || !strcmp(text, "1/4")) whole = 0.25;
		else if(gpif_contains_ci(text, "eighth") || !strcmp(text, "8") || !strcmp(text, "1/8")) whole = 0.125;
		else if(gpif_contains_ci(text, "16th") || !strcmp(text, "16") || !strcmp(text, "1/16")) whole = 1.0 / 16.0;
		else if(gpif_contains_ci(text, "32nd") || !strcmp(text, "32") || !strcmp(text, "1/32")) whole = 1.0 / 32.0;
		else if(gpif_contains_ci(text, "64th") || !strcmp(text, "64") || !strcmp(text, "1/64")) whole = 1.0 / 64.0;
		free(text);
	}

	dot = gpif_find_first(rhythm, "AugmentationDot");
	if(dot)
	{
		xmlChar *count = xmlGetProp(dot, BAD_CAST "count");
		if(count) { dot_count = atoi((const char *)count); xmlFree(count); }
	}
	if(!dot_count)
	{
		xmlNodePtr grouping = gpif_find_first(rhythm, "PrimaryGrouping");
		if(grouping)
		{
			xmlChar *type = xmlGetProp(grouping, BAD_CAST "type");
			char *grouping_text = gpif_node_text(grouping);
			if((type && !gpif_stricmp((const char *)type, "Dotted")) ||
			   (grouping_text && !gpif_stricmp(grouping_text, "Dotted")))
				dot_count = 1;
			if(type) xmlFree(type);
			free(grouping_text);
		}
	}
	if(dot_count == 1) whole *= 1.5;
	else if(dot_count >= 2) whole *= 1.75;

	tuplet = gpif_find_first(rhythm, "PrimaryTuplet");
	if(tuplet)
	{
		xmlChar *num = xmlGetProp(tuplet, BAD_CAST "num"), *den = xmlGetProp(tuplet, BAD_CAST "den");
		if(num) { tnum = atoi((const char *)num); xmlFree(num); }
		if(den) { tden = atoi((const char *)den); xmlFree(den); }
	}
	if(tnum > 0 && tden > 0 && tnum != tden) whole *= (double)tden / (double)tnum;
	result = whole * (double)denominator;
	return (result > 0.0) ? result : ((double)denominator / 4.0);
}

static double gpif_beatpos_to_ms(double beatpos)
{
	unsigned long beat;
	double partial, len;
	if(beatpos < 0.0 || !eof_song || !eof_song->beats)
		return 0.0;
	beat = (unsigned long)floor(beatpos);
	partial = beatpos - (double)beat;
	while(beat + 1UL >= eof_song->beats)
	{
		if(!eof_song_append_beats(eof_song, 1))
			break;
	}
	if(beat >= eof_song->beats)
		beat = eof_song->beats - 1UL;
	if(beat + 1UL < eof_song->beats)
		len = eof_song->beat[beat + 1UL]->fpos - eof_song->beat[beat]->fpos;
	else
		len = (beat > 0) ? eof_song->beat[beat]->fpos - eof_song->beat[beat - 1UL]->fpos : 500.0;
	return eof_song->beat[beat]->fpos + len * partial;
}

typedef struct
{
	int offset;       /* 0..100 percent through the note */
	int quartersteps; /* EOF bend height unit */
} GPIF_BEND_POINT;

static int gpif_bend_point_compare(const void *a, const void *b)
{
	const GPIF_BEND_POINT *pa = a, *pb = b;
	return (pa->offset > pb->offset) - (pa->offset < pb->offset);
}

static int gpif_quartersteps_from_value(int value)
{
	/* GPIF bend values use 25 units per quarter step.  Note bends normally
	 * stay non-negative; clamp negative values because EOF stores the bend
	 * height as an unsigned absolute height above the authored fret. */
	if(value <= 0) return 0;
	value = (value + 12) / 25;
	if(value > 0x7F) value = 0x7F;
	return value;
}

static int gpif_add_bend_curve(xmlNodePtr note_node, EOF_PRO_GUITAR_TRACK *tp, EOF_PRO_GUITAR_NOTE *np,
                               int eof_string, unsigned char voice, double start_ms, double end_ms, int single_note)
{
	GPIF_BEND_POINT points[8];
	int count = 0, origin, destination, middle, origin_off, middle_off1, middle_off2, dest_off;
	int has_origin_off, has_middle, has_middle_off1, has_middle_off2, has_dest_off;
	int i, max_height = 0;

	if(!gpif_property_int(note_node, "BendOriginValue", &origin) ||
	   !gpif_property_int(note_node, "BendDestinationValue", &destination))
		return 1;

	has_origin_off = gpif_property_int(note_node, "BendOriginOffset", &origin_off);
	has_middle = gpif_property_int(note_node, "BendMiddleValue", &middle);
	has_middle_off1 = gpif_property_int(note_node, "BendMiddleOffset1", &middle_off1);
	has_middle_off2 = gpif_property_int(note_node, "BendMiddleOffset2", &middle_off2);
	has_dest_off = gpif_property_int(note_node, "BendDestinationOffset", &dest_off);

	points[count++] = (GPIF_BEND_POINT){0, gpif_quartersteps_from_value(origin)};
	if(has_origin_off) points[count++] = (GPIF_BEND_POINT){origin_off, gpif_quartersteps_from_value(origin)};
	if(has_middle)
	{
		points[count++] = (GPIF_BEND_POINT){has_middle_off1 ? middle_off1 : 50, gpif_quartersteps_from_value(middle)};
		points[count++] = (GPIF_BEND_POINT){has_middle_off2 ? middle_off2 : 50, gpif_quartersteps_from_value(middle)};
	}
	if(has_dest_off && dest_off < 100)
		points[count++] = (GPIF_BEND_POINT){dest_off, gpif_quartersteps_from_value(destination)};
	points[count++] = (GPIF_BEND_POINT){100, gpif_quartersteps_from_value(destination)};

	for(i = 0; i < count; i++)
	{
		if(points[i].offset < 0) points[i].offset = 0;
		if(points[i].offset > 100) points[i].offset = 100;
	}
	qsort(points, (size_t)count, sizeof(points[0]), gpif_bend_point_compare);

	/* Match the legacy importer: keep the note-level summarized bend strength
	 * and create per-string tech notes for the actual curve. */
	for(i = 0; i < count; i++)
		if(points[i].quartersteps > max_height) max_height = points[i].quartersteps;
	if(max_height)
	{
		np->bendstrength = (unsigned char)(0x80 | max_height);
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION;
		if(single_note) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_BEND;
	}

	for(i = 0; i < count; i++)
	{
		EOF_PRO_GUITAR_NOTE *tech;
		/* Drop only exact duplicates introduced by missing/identical offsets.
		 * Equal heights at different offsets are kept because they represent
		 * a plateau in the bend curve. */
		if(i && points[i].offset == points[i - 1].offset &&
		   points[i].quartersteps == points[i - 1].quartersteps)
			continue;
		tech = eof_pro_guitar_track_add_tech_note(tp);
		if(!tech) return 0;
		tech->flags = EOF_PRO_GUITAR_NOTE_FLAG_BEND | EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION;
		tech->bendstrength = (unsigned char)(0x80 | points[i].quartersteps);
		tech->note = (unsigned char)(1U << eof_string);
		tech->type = voice;
		tech->pos = (unsigned long)llround(start_ms + ((end_ms - start_ms) * ((double)points[i].offset / 100.0)));
		tech->length = 1;
	}
	return 1;
}

static int gpif_apply_note_effects(xmlNodePtr note_node, xmlNodePtr beat_node, EOF_PRO_GUITAR_TRACK *tp,
                                   EOF_PRO_GUITAR_NOTE *np, int eof_string, unsigned char voice,
                                   double start_ms, double end_ms, int single_note)
{
	xmlNodePtr direct;
	int value;
	char *text;
	unsigned char bit = (unsigned char)(1U << eof_string);

	if(gpif_find_first(note_node, "Vibrato")) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO;
	if(gpif_property_enabled(note_node, "PalmMuted")) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE;
	if(gpif_property_enabled(note_node, "HopoOrigin"))
	{
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_LINKNEXT;
		np->tflags |= GPIF_TFLAG_HOPO_ORIGIN;
	}
	if(gpif_property_enabled(note_node, "Muted"))
	{
		np->frets[eof_string] |= 0x80;
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_STRING_MUTE;
	}

	direct = gpif_find_first(note_node, "AntiAccent");
	text = gpif_node_text(direct);
	if(text)
	{
		if(!gpif_stricmp(text, "Normal") && eof_gp_import_keep_ghost_guitar_status) np->ghost |= bit;
		free(text);
	}
	direct = gpif_find_first(note_node, "Accent");
	if(direct)
	{
		text = gpif_node_text(direct);
		if(text)
		{
			value = atoi(text);
			if(value == 1) np->tflags |= GPIF_TFLAG_STACCATO;
			else if(value == 4 || value == 8) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_ACCENT;
			free(text);
		}
	}

	if(gpif_property_int(note_node, "Slide", &value) && value)
	{
		if(value & 0x01) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN;
		if(value & 0x02) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN | EOF_PRO_GUITAR_NOTE_FLAG_LINKNEXT;
		if(value & 0x04)
		{
			np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE | EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION;
			np->unpitchend = (np->frets[eof_string] & 0x7F) > eof_gp_import_slide_out_fret_count ?
			                 (np->frets[eof_string] & 0x7F) - eof_gp_import_slide_out_fret_count : 1;
		}
		if(value & 0x08)
		{
			np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE | EOF_PRO_GUITAR_NOTE_FLAG_RS_NOTATION;
			np->unpitchend = (np->frets[eof_string] & 0x7F) + eof_gp_import_slide_out_fret_count;
		}
		if(value & (0x10 | 0x20))
		{
			np->tflags |= EOF_NOTE_TFLAG_SLIDE_IN;
			np->flags |= EOF_NOTE_FLAG_HIGHLIGHT;
		}
	}

	text = gpif_property_text(note_node, "HarmonicType");
	if(text)
	{
		if(!gpif_stricmp(text, "Natural"))
			np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC;
		else if(!eof_gp_import_nat_harmonics_only)
		{
			if(!gpif_stricmp(text, "Pinch"))
				np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_P_HARMONIC;
			else
				np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_HARMONIC; /* artificial/semi/other */
		}
		free(text);
	}

	if(gpif_property_enabled(note_node, "Bended"))
	{
		if(!gpif_add_bend_curve(note_node, tp, np, eof_string, voice, start_ms, end_ms, single_note))
			return 0;
	}

	if(gpif_find_first(note_node, "Trill")) np->flags |= EOF_NOTE_FLAG_IS_TRILL;

	/* Beat-level effects common on bass. */
	if(gpif_property_enabled(beat_node, "Slapped")) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_SLAP;
	if(gpif_property_enabled(beat_node, "Popped")) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_POP;
	if(gpif_find_first(beat_node, "Tremolo")) np->flags |= EOF_NOTE_FLAG_IS_TREMOLO;
	return 1;
}

static void gpif_apply_staccato(EOF_PRO_GUITAR_NOTE *np)
{
	int gems;
	if(!np || !(np->tflags & GPIF_TFLAG_STACCATO)) return;
	if(eof_gp_import_remove_accent_from_staccato)
		np->flags &= ~EOF_PRO_GUITAR_NOTE_FLAG_ACCENT;
	gems = eof_note_count_colors_bitmask(np->note);
	if(!(np->flags & EOF_NOTE_FLAG_IS_TREMOLO) &&
	   !(np->flags & (EOF_PRO_GUITAR_NOTE_FLAG_BEND | EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP |
	                  EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN | EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO |
	                  EOF_PRO_GUITAR_NOTE_FLAG_UNPITCH_SLIDE)) &&
	   !(np->tflags & EOF_NOTE_TFLAG_SLIDE_IN))
	{
		if((gems == 1 && eof_gp_import_truncate_short_notes) ||
		   (gems > 1 && eof_gp_import_truncate_short_chords))
			np->length = 1;
	}
	np->tflags &= ~GPIF_TFLAG_STACCATO;
}

static int gpif_add_beat_text_event(struct eof_guitar_pro_struct *gp, const char *text, unsigned long beat)
{
	EOF_TEXT_EVENT *event;
	const char *section;
	if(!gp || !text || !*text || gp->text_events >= EOF_MAX_TEXT_EVENTS) return 1;
	section = eof_rs_section_text_valid((char *)text);
	if(!section && eof_gp_import_preference_1) return 1;
	event = calloc(1, sizeof(*event));
	if(!event) return 0;
	(void)snprintf(event->text, sizeof(event->text), "%s", section ? section : text);
	event->pos = beat;
	event->track = 0;
	event->flags = EOF_EVENT_FLAG_RS_PHRASE | (section ? EOF_EVENT_FLAG_RS_SECTION : 0);
	event->is_temporary = 1; /* already a beat index, not a measure index */
	gp->text_event[gp->text_events++] = event;
	return 1;
}

static int gpif_apply_beat_text(struct eof_guitar_pro_struct *gp, const GPIF_INDEX *idx, xmlNodePtr beat_node,
                                EOF_PRO_GUITAR_NOTE *np, int instrument, double absolute_beat)
{
	xmlNodePtr text_ref, text_node;
	char *id, *text = NULL;
	text_ref = gpif_find_first(beat_node, "TextId");
	id = gpif_node_text(text_ref);
	if(id && *id)
	{
		text_node = gpif_map_get(&idx->texts, id);
		if(text_node)
		{
			text = gpif_get_text(text_node, "Name");
			if(!text || !*text)
			{
				free(text);
				text = gpif_node_text(text_node);
			}
		}
	}
	free(id);
	if(!text || !*text) { free(text); return 1; }

	if(np && eof_gp_import_text_techniques)
	{
		if(!gpif_stricmp(text, "T"))
		{
			if(instrument == 2) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_SLAP;
			else if(instrument == 1) np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_TAP;
		}
		else if(!gpif_stricmp(text, "P") && instrument == 2)
			np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_POP;
		else if(!gpif_stricmp(text, "S") && instrument == 2)
			np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_SLAP;
	}
	if(eof_gp_import_text)
	{
		unsigned long beat = (unsigned long)floor(absolute_beat + 0.5);
		if(!gpif_add_beat_text_event(gp, text, beat)) { free(text); return 0; }
	}
	free(text);
	return 1;
}

static void gpif_finalize_connected_effects(EOF_PRO_GUITAR_TRACK *tp)
{
	unsigned long i, j;
	if(!tp) return;
	eof_pro_guitar_track_sort_notes(tp);
	for(i = 0; i < tp->notes; i++)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->note[i], *next = NULL;
		for(j = i + 1; j < tp->notes; j++)
		{
			if(tp->note[j]->type != np->type) continue;
			if(tp->note[j]->pos <= np->pos) continue;
			next = tp->note[j];
			break;
		}
		if(next)
		{
			unsigned char shared = np->note & next->note;
			int s;
			if((np->tflags & GPIF_TFLAG_HOPO_ORIGIN) && shared)
			{
				for(s = 0; s < 6; s++)
				{
					if(shared & (1U << s))
					{
						int a = np->frets[s] & 0x7F, b = next->frets[s] & 0x7F;
						next->flags |= (b < a) ? EOF_PRO_GUITAR_NOTE_FLAG_PO : EOF_PRO_GUITAR_NOTE_FLAG_HO;
						break;
					}
				}
			}
			if((np->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP) && (np->flags & EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN) && shared)
			{
				for(s = 0; s < 6; s++)
				{
					if(shared & (1U << s))
					{
						int a = np->frets[s] & 0x7F, b = next->frets[s] & 0x7F;
						if(b > a) np->flags &= ~EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_DOWN;
						else if(b < a) np->flags &= ~EOF_PRO_GUITAR_NOTE_FLAG_SLIDE_UP;
						break;
					}
				}
			}
		}
		np->tflags &= ~GPIF_TFLAG_HOPO_ORIGIN;
	}
}

static int gpif_parse_track_notes(struct eof_guitar_pro_struct *gp, GPIF_TRACK_CONTEXT *ctx,
                                  const GPIF_INDEX *idx, unsigned long track_index,
                                  const unsigned long *measure_start)
{
	unsigned long m;
	EOF_PRO_GUITAR_TRACK *tp = ctx->tp;

	for(m = 0; m < gp->measures; m++)
	{
		GPIF_STRING_LIST barids, voiceids;
		xmlNodePtr bars_node, bar_node;
		unsigned int den = gp->measure[m].den ? gp->measure[m].den : 4U;
		size_t v;

		bars_node = gpif_find_first(idx->master_bars.item[m], "Bars");
		barids = gpif_id_list(bars_node, NULL);
		if(track_index >= barids.count || !strcmp(barids.item[track_index], "-1"))
		{
			gpif_string_list_destroy(&barids);
			continue;
		}
		bar_node = gpif_map_get(&idx->bars, barids.item[track_index]);
		gpif_string_list_destroy(&barids);
		if(!bar_node) continue;

		voiceids = gpif_id_list(gpif_find_first(bar_node, "Voices"), NULL);
		for(v = 0; v < voiceids.count && v < 2U; v++)
		{
			xmlNodePtr voice_node;
			GPIF_STRING_LIST beatids;
			double offset = 0.0;
			size_t b;
			if(!strcmp(voiceids.item[v], "-1")) continue;
			voice_node = gpif_map_get(&idx->voices, voiceids.item[v]);
			if(!voice_node) continue;
			beatids = gpif_id_list(gpif_find_first(voice_node, "Beats"), NULL);
			for(b = 0; b < beatids.count; b++)
			{
				xmlNodePtr beat_node = gpif_map_get(&idx->beats, beatids.item[b]);
				double duration_beats, start_ms, end_ms;
				GPIF_STRING_LIST noteids;
				EOF_PRO_GUITAR_NOTE *np = NULL;
				size_t n;
				int playable_count = 0;

				if(!beat_node || !strcmp(beatids.item[b], "-1")) continue;
				duration_beats = gpif_rhythm_length_beats(beat_node, idx, den);
				start_ms = gpif_beatpos_to_ms((double)measure_start[m] + offset);
				end_ms = gpif_beatpos_to_ms((double)measure_start[m] + offset + duration_beats);
				if(end_ms <= start_ms) end_ms = start_ms + 1.0;

				noteids = gpif_id_list(gpif_find_first(beat_node, "Notes"), "Note");
				for(n = 0; n < noteids.count; n++)
				{
					xmlNodePtr note_node = gpif_map_get(&idx->notes, noteids.item[n]);
					int native_string, eof_string, fret, dead, tie = 0;
					xmlNodePtr tie_node;
					xmlChar *dest;
					if(!note_node || !gpif_property_int(note_node, "String", &native_string)) continue;
					if(native_string < 0 || native_string >= ctx->native_strings || native_string >= GPIF_MAX_NATIVE_STRINGS) continue;
					eof_string = ctx->string_map[native_string];
					if(eof_string < 0 || eof_string >= 6) continue;

					tie_node = gpif_find_first(note_node, "Tie");
					dest = tie_node ? xmlGetProp(tie_node, BAD_CAST "destination") : NULL;
					if(dest)
					{
						tie = (!gpif_stricmp((const char *)dest, "true") || !strcmp((const char *)dest, "1"));
						xmlFree(dest);
					}
					if(tie && ctx->previous[v][eof_string])
					{
						EOF_PRO_GUITAR_NOTE *prev = ctx->previous[v][eof_string];
						long desired;
						if(!gpif_apply_note_effects(note_node, beat_node, tp, prev, eof_string, (unsigned char)v,
						                            start_ms, end_ms, noteids.count == 1U))
						{
							gpif_string_list_destroy(&noteids); gpif_string_list_destroy(&beatids); gpif_string_list_destroy(&voiceids); return 0;
						}
						desired = (long)llround(end_ms) - (long)prev->pos;
						if(desired > prev->length) prev->length = desired;
						continue;
					}

					if(!gpif_property_int(note_node, "Fret", &fret))
						fret = ctx->previous_valid[v][eof_string] ? ctx->previous_fret[v][eof_string] : 0;
					if(fret < 0) fret = 0;
					if(fret > 127) fret = 127;
					if(!np)
					{
						np = eof_pro_guitar_track_add_note(tp);
						if(!np) { gpif_string_list_destroy(&noteids); gpif_string_list_destroy(&beatids); gpif_string_list_destroy(&voiceids); return 0; }
						np->type = (unsigned char)v;
						np->pos = (unsigned long)llround(start_ms);
						np->length = (long)llround(end_ms - start_ms);
						if(np->length < 1) np->length = 1;
					}
					np->note |= (unsigned char)(1U << eof_string);
					np->frets[eof_string] = (unsigned char)fret;
					dead = gpif_property_enabled(note_node, "Muted");
					if(dead) np->frets[eof_string] |= 0x80;
					playable_count++;

					ctx->previous[v][eof_string] = np;
					ctx->previous_fret[v][eof_string] = (unsigned char)fret;
					ctx->previous_valid[v][eof_string] = 1;
					if(!gpif_apply_note_effects(note_node, beat_node, tp, np, eof_string, (unsigned char)v,
					                            start_ms, end_ms, noteids.count == 1U))
					{
						gpif_string_list_destroy(&noteids); gpif_string_list_destroy(&beatids); gpif_string_list_destroy(&voiceids); return 0;
					}
				}

				if(np && playable_count)
				{
					xmlNodePtr chord_ref = gpif_find_first(beat_node, "ChordId");
					char *cid = gpif_node_text(chord_ref);
					if(cid)
					{
						xmlNodePtr chord = gpif_map_get(&idx->chords, cid);
						char *cname = gpif_get_text(chord, "Name");
						if(cname && *cname) (void)snprintf(np->name, sizeof(np->name), "%s", cname);
						free(cname);
						free(cid);
					}
					gpif_apply_staccato(np);
				}
				if(!gpif_apply_beat_text(gp, idx, beat_node, np, gp->instrument_types[track_index],
				                         (double)measure_start[m] + offset))
				{
					gpif_string_list_destroy(&noteids); gpif_string_list_destroy(&beatids); gpif_string_list_destroy(&voiceids); return 0;
				}
				gpif_string_list_destroy(&noteids);
				offset += duration_beats;
			}
			gpif_string_list_destroy(&beatids);
		}
		gpif_string_list_destroy(&voiceids);
	}
	return 1;
}

static int gpif_parse_measures(struct eof_guitar_pro_struct *gp, const GPIF_INDEX *idx,
                               unsigned long **measure_start_out, char *undo_made, char *import_ts_out)
{
	unsigned long i, totalbeats = 0, beatctr = 0, existing_ts = 0;
	unsigned long *measure_start;
	unsigned current_num = 4, current_den = 4, last_num = 0, last_den = 0;
	char import_ts = 0, overwrite_ts = 0;

	gp->measures = (unsigned long)idx->master_bars.count;
	gp->measure = calloc(gp->measures, sizeof(*gp->measure));
	measure_start = calloc(gp->measures + 1UL, sizeof(*measure_start));
	if(!gp->measure || !measure_start)
	{
		free(measure_start);
		return 0;
	}
	for(i = 0; i < gp->measures; i++)
	{
		xmlNodePtr mb = idx->master_bars.item[i], repeat, section, endings;
		char *time = gpif_get_text(mb, "Time"), *section_text = NULL;
		if(time)
		{
			unsigned n, d;
			if(sscanf(time, "%u/%u", &n, &d) == 2 && n > 0 && n <= 255 && d > 0 && d <= 255)
			{
				current_num = n;
				current_den = d;
			}
			free(time);
		}
		gp->measure[i].num = (unsigned char)current_num;
		gp->measure[i].den = (unsigned char)current_den;
		gp->measure[i].start_of_repeat = (i == 0) ? 1 : 0;
		measure_start[i] = totalbeats;
		totalbeats += current_num;

		repeat = gpif_find_first(mb, "Repeat");
		if(repeat)
		{
			xmlChar *start = xmlGetProp(repeat, BAD_CAST "start"), *count = xmlGetProp(repeat, BAD_CAST "count");
			if(start)
			{
				if(!gpif_stricmp((const char *)start, "true") || !strcmp((const char *)start, "1")) gp->measure[i].start_of_repeat = 1;
				xmlFree(start);
			}
			if(count)
			{
				int c = atoi((const char *)count);
				if(c > 0 && c < 256) gp->measure[i].num_of_repeats = (unsigned char)c;
				xmlFree(count);
			}
		}
		else
		{
			char *rs = gpif_get_text(mb, "RepeatStart"), *rc = gpif_get_text(mb, "RepeatCount");
			if(rs) { if(!gpif_stricmp(rs, "true") || !strcmp(rs, "1")) gp->measure[i].start_of_repeat = 1; free(rs); }
			if(rc) { int c = atoi(rc); if(c > 0 && c < 256) gp->measure[i].num_of_repeats = (unsigned char)c; free(rc); }
		}

		endings = gpif_find_first(mb, "AlternateEndings");
		if(endings)
		{
			char *text = gpif_node_text(endings), *p = text;
			unsigned mask = 0;
			while(p && *p)
			{
				char *end;
				long n;
				while(*p && !isdigit((unsigned char)*p)) p++;
				if(!*p) break;
				n = strtol(p, &end, 10);
				if(n >= 1 && n <= 8) mask |= 1U << (n - 1);
				p = end;
			}
			gp->measure[i].alt_endings = (unsigned char)mask;
			free(text);
		}

		section = gpif_find_first(mb, "Section");
		if(section)
		{
			section_text = gpif_get_text(section, "Text");
			if(!section_text || !*section_text)
			{
				free(section_text);
				section_text = gpif_get_text(section, "Letter");
			}
			if(!section_text || !*section_text)
			{
				free(section_text);
				section_text = gpif_node_text(section);
			}
			if(eof_gp_import_text && section_text && *section_text)
				(void)gpif_add_text_event(gp, section_text, i);
			free(section_text);
		}
	}
	measure_start[gp->measures] = totalbeats;
	*measure_start_out = measure_start;

	while(eof_song->beats < totalbeats + 2UL)
	{
		if(!eof_song_append_beats(eof_song, 1)) return 0;
	}
	eof_chart_length = eof_song->beat[eof_song->beats - 1UL]->pos;

	if(eof_use_ts && !eof_song->tags->tempo_map_locked)
	{
		import_ts = 1;
		for(i = 0; i < eof_song->beats; i++)
			if(eof_get_ts(eof_song, NULL, NULL, i) == 1) existing_ts++;
		if(existing_ts)
		{
			for(i = 0; i < gp->measures; i++)
			{
				unsigned n = 0, d = 0;
				eof_get_effective_ts(eof_song, &n, &d, measure_start[i], 1);
				if(n != gp->measure[i].num || d != gp->measure[i].den)
				{
					eof_clear_input();
					if(alert(NULL, "Import GP7/8 file's differing time signatures?", NULL, "&Yes", "&No", 'y', 'n') == 1)
						overwrite_ts = 1;
					else
						import_ts = 0;
					break;
				}
			}
		}
		else
			overwrite_ts = 1;
	}

	if(import_ts)
	{
		if(overwrite_ts && existing_ts && undo_made && !*undo_made)
		{
			eof_prepare_undo(EOF_UNDO_TYPE_NONE);
			*undo_made = 1;
		}
		for(i = 0, beatctr = 0; i < gp->measures; i++)
		{
			unsigned b;
			for(b = 0; b < gp->measure[i].num; b++, beatctr++)
			{
				if(!b && ((gp->measure[i].num != last_num) || (gp->measure[i].den != last_den)))
					(void)eof_apply_ts(gp->measure[i].num, gp->measure[i].den, beatctr, eof_song, 0);
				else if(overwrite_ts)
					eof_song->beat[beatctr]->flags &= (EOF_BEAT_FLAG_ANCHOR | EOF_BEAT_FLAG_EVENTS | EOF_BEAT_FLAG_KEY_SIG);
			}
			last_num = gp->measure[i].num;
			last_den = gp->measure[i].den;
		}
		eof_calculate_beats(eof_song);
	}

	/* Convert section event positions from measure number to beat number, as the
	 * legacy importer does before repeat unwrapping. */
	for(i = 0; i < gp->text_events; i++)
	{
		unsigned long m = gp->text_event[i]->pos;
		if(m < gp->measures)
		{
			gp->text_event[i]->pos = measure_start[m];
			gp->text_event[i]->is_temporary = 1;
		}
	}
	*import_ts_out = import_ts;
	return 1;
}

/* ---------- public loader ---------- */

struct eof_guitar_pro_struct *eof_load_gpif(const char *fn, char *undo_made)
{
	unsigned char *xml_data = NULL;
	size_t xml_size = 0;
	char error[512] = {0};
	xmlDocPtr doc = NULL;
	xmlNodePtr root;
	GPIF_INDEX idx;
	struct eof_guitar_pro_struct *gp = NULL;
	GPIF_TRACK_CONTEXT *ctx = NULL;
	unsigned long *measure_start = NULL;
	unsigned long i;
	int wide_choice = -1;
	char import_ts = 0;

	memset(&idx, 0, sizeof(idx));
	if(!fn || !eof_song)
		return NULL;

	eof_log("eof_load_gpif() entered", 1);
	if(!gpif_zip_extract_score(fn, &xml_data, &xml_size, error, sizeof(error)))
	{
		eof_log(error, 1);
		allegro_message("Unable to import GP7/8 file:\n%s", error);
		return NULL;
	}

	/* XML_PARSE_NONET blocks network access.  Deliberately do NOT request
	 * XML_PARSE_NOENT, so external/general entities are not expanded. */
	doc = xmlReadMemory((const char *)xml_data, (int)xml_size, "score.gpif", NULL,
	                    XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	free(xml_data);
	if(!doc)
	{
		allegro_message("Unable to parse Content/score.gpif (invalid XML).");
		return NULL;
	}
	root = xmlDocGetRootElement(doc);
	if(!root || !gpif_index_walk(root, &idx))
	{
		allegro_message("Unable to index GPIF XML (out of memory or invalid document).");
		goto fail;
	}
	if(!idx.tracks.count || !idx.master_bars.count)
	{
		allegro_message("The GPIF file contains no tracks or master bars.");
		goto fail;
	}

	gp = calloc(1, sizeof(*gp));
	if(!gp) goto fail;
	gp->fileversion = 800; /* GPIF family marker; legacy parser maxes at 510 */
	gp->numtracks = (unsigned long)idx.tracks.count;
	gp->names = calloc(gp->numtracks, sizeof(*gp->names));
	gp->instrument_types = calloc(gp->numtracks, sizeof(*gp->instrument_types));
	gp->track = calloc(gp->numtracks, sizeof(*gp->track));
	ctx = calloc(gp->numtracks, sizeof(*ctx));
	if(!gp->names || !gp->instrument_types || !gp->track || !ctx) goto fail;
	for(i = 0; i < 19; i++) gp->symbols[i] = 0xFFFFU;

	for(i = 0; i < gp->numtracks; i++)
	{
		if(!gpif_prepare_track(gp, &ctx[i], idx.tracks.item[i], i, &wide_choice))
			goto fail;
	}
	if(!gpif_parse_measures(gp, &idx, &measure_start, undo_made, &import_ts))
		goto fail;
	for(i = 0; i < gp->numtracks; i++)
	{
		if(!gpif_parse_track_notes(gp, &ctx[i], &idx, i, measure_start))
			goto fail;
	}

	/* Reuse EOF's existing repeat/alternate-ending unwrapping. */
	for(i = 0; i < gp->numtracks; i++)
	{
		if(eof_unwrap_gp_track(gp, i, import_ts, 0))
		{
			(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1,
			               "Warning: Failed to unwrap repeats for GPIF track #%lu (%s)", i + 1UL, gp->names[i]);
			eof_log(eof_log_string, 1);
		}
		import_ts = 0;
	}

	for(i = 0; i < gp->numtracks; i++)
	{
		unsigned long n;
		gpif_finalize_connected_effects(gp->track[i]);
		eof_build_trill_phrases(gp->track[i]);
		if((eof_song->track[eof_selected_track]->flags & EOF_TRACK_FLAG_UNLIMITED_DIFFS) || !eof_gp_import_replaces_track)
		{
			eof_build_tremolo_phrases(gp->track[i], 0);
			eof_build_tremolo_phrases(gp->track[i], 1);
		}
		else
			eof_build_tremolo_phrases(gp->track[i], 0xFF);
		eof_pro_guitar_track_fix_fingerings(gp->track[i], NULL, 0);
		for(n = 0; n < gp->track[i]->notes; n++)
			eof_sanitize_note_flags(&gp->track[i]->note[n]->flags, EOF_TRACK_PRO_GUITAR, EOF_TRACK_PRO_GUITAR);
	}

	free(ctx);
	free(measure_start);
	gpif_index_destroy(&idx);
	xmlFreeDoc(doc);
	eof_log("GPIF import successful", 1);
	return gp;

fail:
	free(ctx);
	free(measure_start);
	gpif_free_gp(gp);
	gpif_index_destroy(&idx);
	if(doc) xmlFreeDoc(doc);
	return NULL;
}

#endif /* EOF_BUILD */
