/*
 * Editor On Fire - native TuxGuitar 2.x importer
 *
 * TuxGuitar 2.x stores .tg files as ZIP containers with two members:
 *   version.txt  ("TuxGuitar_file_format 2.0.0")
 *   content.xml  (TuxGuitarFile/TGSong)
 *
 * This reader deliberately consumes that native representation directly,
 * instead of requiring TuxGuitar to be installed or launching a converter.
 */

#ifdef EOF_BUILD

#include <allegro.h>
#include <ctype.h>
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
#include "main.h"
#include "note.h"
#include "song.h"
#include "tg_import.h"
#include "undo.h"
#include "utility.h"

/* EOF uses this internally, but some revisions don't publish the prototype. */
extern int eof_apply_ts(unsigned num, unsigned den, unsigned long beat, EOF_SONG *sp, char undo);

#define TG_VERSION_PATH "version.txt"
#define TG_CONTENT_PATH "content.xml"
#define TG_VERSION_PREFIX "TuxGuitar_file_format "
#define TG_MAX_XML_SIZE (128UL * 1024UL * 1024UL)
#define TG_MAX_CONTAINER_SIZE (512UL * 1024UL * 1024UL)
#define TG_ZIP_READ_CHUNK (64UL * 1024UL)
#define TG_PRECISE_WHOLE 46126080LL
#define TG_PRECISE_QUARTER 11531520LL

typedef struct
{
	uint16_t method;
	uint16_t flags;
	uint32_t crc32_value;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint32_t local_header_offset;
} TG_ZIP_ENTRY;

typedef struct
{
	int id;
	int bank;
	int program;
} TG_CHANNEL;

typedef struct
{
	unsigned num, den;
	double quarter_bpm;
	long long precise_start;
	long long precise_length;
	double ms_start;
	unsigned long eof_beat_start;
} TG_MEASURE_INFO;

static char *tg_strdup(const char *src)
{
	char *ret;
	size_t len;
	if(!src)
		return NULL;
	len = strlen(src);
	ret = malloc(len + 1U);
	if(ret)
		memcpy(ret, src, len + 1U);
	return ret;
}

static void tg_set_error(char *buffer, size_t size, const char *message)
{
	if(buffer && size)
	{
		(void)snprintf(buffer, size, "%s", message ? message : "Unknown TuxGuitar import error");
		buffer[size - 1U] = '\0';
	}
}

static uint16_t tg_le16(const unsigned char *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t tg_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int tg_read_container(const char *fn, unsigned char **data, size_t *data_size, char *error, size_t error_size)
{
	PACKFILE *pf = NULL;
	unsigned char *buffer = NULL;
	size_t used = 0, capacity = TG_ZIP_READ_CHUNK;
	int ok = 0;

	if(data) *data = NULL;
	if(data_size) *data_size = 0;
	if(!fn || !data || !data_size)
		return 0;

	pf = pack_fopen(fn, "rb");
	if(!pf)
	{
		tg_set_error(error, error_size, "Unable to open TuxGuitar file");
		return 0;
	}
	buffer = malloc(capacity);
	if(!buffer)
	{
		tg_set_error(error, error_size, "Out of memory while reading TuxGuitar file");
		goto cleanup;
	}

	for(;;)
	{
		long got, request;
		size_t available;
		if(used == capacity)
		{
			size_t new_capacity;
			unsigned char *resized;
			if(capacity >= TG_MAX_CONTAINER_SIZE)
			{
				tg_set_error(error, error_size, "TuxGuitar container exceeds the 512 MiB safety limit");
				goto cleanup;
			}
			new_capacity = capacity * 2U;
			if((new_capacity < capacity) || (new_capacity > TG_MAX_CONTAINER_SIZE))
				new_capacity = TG_MAX_CONTAINER_SIZE;
			resized = realloc(buffer, new_capacity);
			if(!resized)
			{
				tg_set_error(error, error_size, "Out of memory while reading TuxGuitar file");
				goto cleanup;
			}
			buffer = resized;
			capacity = new_capacity;
		}
		available = capacity - used;
		if(available > TG_ZIP_READ_CHUNK)
			available = TG_ZIP_READ_CHUNK;
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
		tg_set_error(error, error_size, "TuxGuitar file is empty or truncated");
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

static int tg_zip_name_equals(const unsigned char *name, size_t length, const char *wanted)
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

static int tg_zip_find_entry(const unsigned char *zip, size_t zip_size, const char *wanted, TG_ZIP_ENTRY *entry, char *error, size_t error_size)
{
	size_t scan_start, pos, eocd_pos = (size_t)-1;
	const unsigned char *eocd;
	uint16_t disk_no, cd_disk, entries_on_disk, entries;
	uint32_t cd_size, cd_offset;
	unsigned index;

	if(!zip || !wanted || !entry || (zip_size < 22U))
	{
		tg_set_error(error, error_size, "Invalid or truncated TuxGuitar ZIP container");
		return 0;
	}
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
		tg_set_error(error, error_size,
			"This .tg file is not a TuxGuitar 2.x ZIP container. Legacy TuxGuitar 0.7-1.5 files must be re-saved by TuxGuitar 2.x first.");
		return 0;
	}
	eocd = zip + eocd_pos;
	disk_no = tg_le16(eocd + 4);
	cd_disk = tg_le16(eocd + 6);
	entries_on_disk = tg_le16(eocd + 8);
	entries = tg_le16(eocd + 10);
	cd_size = tg_le32(eocd + 12);
	cd_offset = tg_le32(eocd + 16);
	if(disk_no || cd_disk || (entries_on_disk != entries))
	{
		tg_set_error(error, error_size, "Multi-disk TuxGuitar ZIP files are not supported");
		return 0;
	}
	if((entries == 0xFFFFU) || (cd_size == 0xFFFFFFFFUL) || (cd_offset == 0xFFFFFFFFUL))
	{
		tg_set_error(error, error_size, "ZIP64 TuxGuitar containers are not supported");
		return 0;
	}
	if(((uint64_t)cd_offset + (uint64_t)cd_size > (uint64_t)zip_size) || ((size_t)cd_offset >= zip_size))
	{
		tg_set_error(error, error_size, "Invalid TuxGuitar ZIP central directory");
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
			tg_set_error(error, error_size, "Truncated TuxGuitar ZIP central directory");
			return 0;
		}
		hdr = zip + pos;
		if(tg_le32(hdr) != 0x02014B50UL)
		{
			tg_set_error(error, error_size, "Malformed TuxGuitar ZIP central directory");
			return 0;
		}
		fnlen = tg_le16(hdr + 28);
		extralen = tg_le16(hdr + 30);
		commentlen = tg_le16(hdr + 32);
		record_size = 46U + (size_t)fnlen + (size_t)extralen + (size_t)commentlen;
		if(record_size > zip_size - pos)
		{
			tg_set_error(error, error_size, "Truncated TuxGuitar ZIP entry");
			return 0;
		}
		if(tg_zip_name_equals(hdr + 46, fnlen, wanted))
		{
			entry->flags = tg_le16(hdr + 8);
			entry->method = tg_le16(hdr + 10);
			entry->crc32_value = tg_le32(hdr + 16);
			entry->compressed_size = tg_le32(hdr + 20);
			entry->uncompressed_size = tg_le32(hdr + 24);
			entry->local_header_offset = tg_le32(hdr + 42);
			return 1;
		}
		pos += record_size;
	}
	(void)snprintf(error, error_size, "Required TuxGuitar member '%s' was not found", wanted);
	if(error && error_size) error[error_size - 1U] = '\0';
	return 0;
}

static int tg_zip_extract(const unsigned char *zip, size_t zip_size, const char *wanted, unsigned char **out, size_t *out_size, char *error, size_t error_size)
{
	TG_ZIP_ENTRY entry;
	const unsigned char *local, *compressed;
	unsigned char *plain = NULL;
	uint16_t fnlen, extralen;
	size_t data_offset;
	int ok = 0;

	if(out) *out = NULL;
	if(out_size) *out_size = 0;
	if(!zip || !wanted || !out || !out_size)
		return 0;
	memset(&entry, 0, sizeof(entry));
	if(!tg_zip_find_entry(zip, zip_size, wanted, &entry, error, error_size))
		return 0;
	if(entry.flags & 1U)
	{
		tg_set_error(error, error_size, "Encrypted TuxGuitar ZIP entries are not supported");
		return 0;
	}
	if((entry.method != 0U) && (entry.method != 8U))
	{
		tg_set_error(error, error_size, "TuxGuitar ZIP entry uses an unsupported compression method");
		return 0;
	}
	if(!entry.uncompressed_size || (entry.uncompressed_size > TG_MAX_XML_SIZE))
	{
		tg_set_error(error, error_size, "TuxGuitar ZIP member is empty or exceeds the safety limit");
		return 0;
	}
	if(((size_t)entry.local_header_offset > zip_size) || (zip_size - (size_t)entry.local_header_offset < 30U))
	{
		tg_set_error(error, error_size, "Invalid TuxGuitar ZIP local header");
		return 0;
	}
	local = zip + (size_t)entry.local_header_offset;
	if(tg_le32(local) != 0x04034B50UL)
	{
		tg_set_error(error, error_size, "Invalid TuxGuitar ZIP local header signature");
		return 0;
	}
	fnlen = tg_le16(local + 26);
	extralen = tg_le16(local + 28);
	data_offset = (size_t)entry.local_header_offset + 30U + (size_t)fnlen + (size_t)extralen;
	if((data_offset > zip_size) || ((size_t)entry.compressed_size > zip_size - data_offset))
	{
		tg_set_error(error, error_size, "Truncated TuxGuitar ZIP payload");
		return 0;
	}
	compressed = zip + data_offset;
	plain = malloc((size_t)entry.uncompressed_size + 1U);
	if(!plain)
	{
		tg_set_error(error, error_size, "Out of memory while extracting TuxGuitar ZIP member");
		return 0;
	}
	if(entry.method == 0U)
	{
		if(entry.compressed_size != entry.uncompressed_size)
		{
			tg_set_error(error, error_size, "Invalid stored TuxGuitar ZIP entry sizes");
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
			tg_set_error(error, error_size, "Unable to initialize zlib for TuxGuitar import");
			goto cleanup;
		}
		zret = inflate(&zs, Z_FINISH);
		inflateEnd(&zs);
		if((zret != Z_STREAM_END) || (zs.total_out != entry.uncompressed_size))
		{
			tg_set_error(error, error_size, "Unable to inflate TuxGuitar ZIP member");
			goto cleanup;
		}
	}
	if((uint32_t)crc32(0L, plain, entry.uncompressed_size) != entry.crc32_value)
	{
		tg_set_error(error, error_size, "TuxGuitar ZIP member failed CRC-32 validation");
		goto cleanup;
	}
	plain[entry.uncompressed_size] = '\0';
	*out = plain;
	*out_size = entry.uncompressed_size;
	plain = NULL;
	ok = 1;
cleanup:
	free(plain);
	return ok;
}

/* ---------- XML helpers ---------- */

static int tg_node_is(xmlNodePtr node, const char *name)
{
	return node && (node->type == XML_ELEMENT_NODE) && !xmlStrcasecmp(node->name, BAD_CAST name);
}

static xmlNodePtr tg_child(xmlNodePtr node, const char *name)
{
	xmlNodePtr cur;
	if(!node)
		return NULL;
	for(cur = node->children; cur; cur = cur->next)
	{
		if(tg_node_is(cur, name))
			return cur;
	}
	return NULL;
}

static char *tg_node_text(xmlNodePtr node)
{
	xmlChar *text;
	char *ret;
	if(!node)
		return NULL;
	text = xmlNodeGetContent(node);
	if(!text)
		return NULL;
	ret = tg_strdup((const char *)text);
	xmlFree(text);
	return ret;
}

static long long tg_node_ll(xmlNodePtr node, long long fallback)
{
	char *text, *end;
	long long value;
	if(!node)
		return fallback;
	text = tg_node_text(node);
	if(!text)
		return fallback;
	end = NULL;
	value = strtoll(text, &end, 10);
	if(end == text)
		value = fallback;
	free(text);
	return value;
}

static int tg_node_int(xmlNodePtr node, int fallback)
{
	long long value = tg_node_ll(node, fallback);
	if((value < INT_MIN) || (value > INT_MAX))
		return fallback;
	return (int)value;
}

static int tg_child_int(xmlNodePtr node, const char *name, int fallback)
{
	return tg_node_int(tg_child(node, name), fallback);
}

static int tg_attr_int(xmlNodePtr node, const char *name, int fallback)
{
	xmlChar *value;
	char *end;
	long parsed;
	if(!node)
		return fallback;
	value = xmlGetProp(node, BAD_CAST name);
	if(!value)
		return fallback;
	end = NULL;
	parsed = strtol((const char *)value, &end, 10);
	if(end == (char *)value)
		parsed = fallback;
	xmlFree(value);
	if((parsed < INT_MIN) || (parsed > INT_MAX))
		return fallback;
	return (int)parsed;
}

static int tg_attr_true(xmlNodePtr node, const char *name)
{
	xmlChar *value;
	int ret = 0;
	if(!node)
		return 0;
	value = xmlGetProp(node, BAD_CAST name);
	if(value)
	{
		ret = (!xmlStrcasecmp(value, BAD_CAST "true") || !xmlStrcasecmp(value, BAD_CAST "1"));
		xmlFree(value);
	}
	return ret;
}

static int tg_has_child(xmlNodePtr node, const char *name)
{
	return tg_child(node, name) ? 1 : 0;
}

static xmlNodePtr tg_find_song(xmlDocPtr doc)
{
	xmlNodePtr root;
	if(!doc)
		return NULL;
	root = xmlDocGetRootElement(doc);
	if(!root || !tg_node_is(root, "TuxGuitarFile"))
		return NULL;
	return tg_child(root, "TGSong");
}

/* ---------- timing ---------- */

static int tg_count_children(xmlNodePtr parent, const char *name)
{
	xmlNodePtr cur;
	int count = 0;
	for(cur = parent ? parent->children : NULL; cur; cur = cur->next)
		if(tg_node_is(cur, name))
			count++;
	return count;
}

static int tg_parse_measures(xmlNodePtr song, TG_MEASURE_INFO **out, size_t *out_count)
{
	TG_MEASURE_INFO *array;
	xmlNodePtr cur;
	size_t count, index = 0;
	long long precise = TG_PRECISE_QUARTER;
	double ms = 0.0;
	unsigned long eofbeat = 0;

	if(out) *out = NULL;
	if(out_count) *out_count = 0;
	if(!song || !out || !out_count)
		return 0;
	count = (size_t)tg_count_children(song, "TGMeasureHeader");
	if(!count)
		return 0;
	array = calloc(count, sizeof(*array));
	if(!array)
		return 0;

	for(cur = song->children; cur && (index < count); cur = cur->next)
	{
		xmlNodePtr ts, tempo;
		int raw, base;
		double qbpm;
		if(!tg_node_is(cur, "TGMeasureHeader"))
			continue;
		ts = tg_child(cur, "timeSignature");
		array[index].num = (unsigned)tg_attr_int(ts, "numerator", 4);
		array[index].den = (unsigned)tg_attr_int(ts, "denominator", 4);
		if(!array[index].num) array[index].num = 4;
		if(!array[index].den) array[index].den = 4;
		tempo = tg_child(cur, "tempo");
		raw = tg_node_int(tempo, 120);
		base = tg_attr_int(tempo, "base", 4);
		if(raw <= 0) raw = 120;
		if(base <= 0) base = 4;
		qbpm = (double)raw * 4.0 / (double)base;
		if(tg_attr_true(tempo, "dotted"))
			qbpm *= 1.5;
		if(qbpm < 1.0) qbpm = 1.0;
		array[index].quarter_bpm = qbpm;
		array[index].precise_start = precise;
		array[index].precise_length = (TG_PRECISE_WHOLE * (long long)array[index].num) / (long long)array[index].den;
		array[index].ms_start = ms;
		array[index].eof_beat_start = eofbeat;
		precise += array[index].precise_length;
		ms += ((double)array[index].num * 4.0 / (double)array[index].den) * (60000.0 / qbpm);
		eofbeat += array[index].num;
		index++;
	}
	*out = array;
	*out_count = index;
	return index ? 1 : 0;
}

static size_t tg_measure_for_precise(const TG_MEASURE_INFO *measure, size_t count, long long precise)
{
	size_t i;
	if(!measure || !count)
		return 0;
	for(i = 0; i + 1U < count; i++)
	{
		if(precise < measure[i + 1U].precise_start)
			return i;
	}
	return count - 1U;
}

static double tg_precise_to_ms(const TG_MEASURE_INFO *measure, size_t count, long long precise)
{
	size_t index = tg_measure_for_precise(measure, count, precise);
	long long delta = precise - measure[index].precise_start;
	if(delta < 0)
		delta = 0;
	return measure[index].ms_start +
		((double)delta / (double)TG_PRECISE_QUARTER) * (60000.0 / measure[index].quarter_bpm);
}

static long long tg_duration_precise(xmlNodePtr voice)
{
	xmlNodePtr duration, division;
	long long precise;
	int value, enters = 1, times = 1;
	xmlChar *dotted;

	duration = tg_child(voice, "duration");
	if(!duration)
		return 0;
	value = tg_attr_int(duration, "value", 4);
	if((value != 1) && (value != 2) && (value != 4) && (value != 8) &&
	   (value != 16) && (value != 32) && (value != 64))
		value = 4;
	precise = TG_PRECISE_WHOLE / value;
	division = tg_child(duration, "divisionType");
	if(division)
	{
		enters = tg_attr_int(division, "enters", 1);
		times = tg_attr_int(division, "times", 1);
		if(enters < 1) enters = 1;
		if(times < 1) times = 1;
		precise = precise * times / enters;
	}
	dotted = xmlGetProp(duration, BAD_CAST "dotted");
	if(dotted)
	{
		if(!xmlStrcasecmp(dotted, BAD_CAST "dotted"))
			precise = precise * 3LL / 2LL;
		else if(!xmlStrcasecmp(dotted, BAD_CAST "doubleDotted"))
			precise = precise * 7LL / 4LL;
		xmlFree(dotted);
	}
	return precise;
}

static int tg_project_has_notes(void)
{
	unsigned long track;
	if(!eof_song)
		return 0;
	for(track = 1; track < eof_song->tracks; track++)
	{
		if(eof_get_track_size_all(eof_song, track))
			return 1;
	}
	return 0;
}

static int tg_apply_tempo_map(const TG_MEASURE_INFO *measure, size_t count, char *undo_made)
{
	size_t i;
	unsigned long totalbeats = 0, beatindex = 0;
	double firstpos;
	int apply = 1;

	if(!eof_song || !measure || !count)
		return 0;
	if(tg_project_has_notes())
	{
		eof_clear_input();
		apply = (alert(NULL, "Replace the current tempo/time-signature map with the TuxGuitar map?",
			"Imported notes keep TuxGuitar timing either way.", "&Yes", "&No", 'y', 'n') == 1);
	}
	if(!apply)
		return 1;
	if(eof_song->tags->tempo_map_locked)
	{
		eof_clear_input();
		if(alert(NULL, "The tempo map is locked. Unlock it for TuxGuitar import?", NULL, "&Yes", "&No", 'y', 'n') != 1)
			return 1;
		eof_song->tags->tempo_map_locked = 0;
	}
	for(i = 0; i < count; i++)
	{
		if(totalbeats > ULONG_MAX - measure[i].num)
			return 0;
		totalbeats += measure[i].num;
	}
	if(totalbeats < 1U)
		return 0;
	if(undo_made && !*undo_made)
	{
		eof_prepare_undo(EOF_UNDO_TYPE_NONE);
		*undo_made = 1;
	}
	firstpos = eof_song->beats ? eof_song->beat[0]->fpos : 0.0;
	if(!eof_song_resize_beats(eof_song, totalbeats + 1UL))
		return 0;

	for(i = 0; i < eof_song->beats; i++)
	{
		eof_song->beat[i]->flags &= ~(EOF_BEAT_FLAG_ANCHOR | EOF_BEAT_FLAG_START_2_4 | EOF_BEAT_FLAG_START_3_4 |
			EOF_BEAT_FLAG_START_4_4 | EOF_BEAT_FLAG_START_5_4 | EOF_BEAT_FLAG_START_6_4 | EOF_BEAT_FLAG_CUSTOM_TS);
	}
	eof_song->beat[0]->fpos = firstpos;
	eof_song->beat[0]->pos = firstpos + 0.5;
	for(i = 0, beatindex = 0; i < count; i++)
	{
		unsigned b;
		double ppqn_d = 60000000.0 / measure[i].quarter_bpm;
		unsigned long ppqn = (unsigned long)floor(ppqn_d + 0.5);
		if(!ppqn) ppqn = 1;
		(void)eof_apply_ts(measure[i].num, measure[i].den, beatindex, eof_song, 0);
		if((i == 0U) || (fabs(measure[i].quarter_bpm - measure[i - 1U].quarter_bpm) > 0.0001))
			eof_song->beat[beatindex]->flags |= EOF_BEAT_FLAG_ANCHOR;
		for(b = 0; b < measure[i].num && (beatindex + b < eof_song->beats); b++)
			eof_song->beat[beatindex + b]->ppqn = ppqn;
		beatindex += measure[i].num;
	}
	eof_song->beat[totalbeats]->ppqn = eof_song->beat[totalbeats - 1UL]->ppqn;
	(void)eof_calculate_beats(eof_song);
	eof_beat_stats_cached = 0;
	return 1;
}

/* ---------- song/track parsing ---------- */

static TG_CHANNEL *tg_parse_channels(xmlNodePtr song, size_t *out_count)
{
	TG_CHANNEL *array = NULL;
	size_t count = 0, capacity = 0;
	xmlNodePtr cur;
	if(out_count) *out_count = 0;
	for(cur = song ? song->children : NULL; cur; cur = cur->next)
	{
		TG_CHANNEL *tmp;
		if(!tg_node_is(cur, "TGChannel"))
			continue;
		if(count >= capacity)
		{
			size_t newcap = capacity ? capacity * 2U : 16U;
			tmp = realloc(array, newcap * sizeof(*tmp));
			if(!tmp)
			{
				free(array);
				return NULL;
			}
			array = tmp;
			capacity = newcap;
		}
		array[count].id = tg_child_int(cur, "id", (int)count);
		array[count].bank = tg_child_int(cur, "bank", 0);
		array[count].program = tg_child_int(cur, "program", 0);
		count++;
	}
	if(out_count) *out_count = count;
	return array;
}

static const TG_CHANNEL *tg_find_channel(const TG_CHANNEL *channels, size_t count, int id)
{
	size_t i;
	for(i = 0; i < count; i++)
		if(channels[i].id == id)
			return &channels[i];
	return NULL;
}

static EOF_PRO_GUITAR_NOTE *tg_alloc_note(unsigned char type, unsigned long pos, long length)
{
	EOF_PRO_GUITAR_NOTE *np = calloc(1, sizeof(*np));
	unsigned i;
	if(!np)
		return NULL;
	np->type = type;
	np->pos = pos;
	np->length = (length > 0) ? length : 1;
	for(i = 0; i < 8U; i++)
		np->frets[i] = 0xFFU;
	return np;
}

static int tg_track_add_note(EOF_PRO_GUITAR_TRACK *tp, EOF_PRO_GUITAR_NOTE *np)
{
	if(!tp || !np || (tp->pgnotes >= EOF_MAX_NOTES))
		return 0;
	tp->pgnote[tp->pgnotes++] = np;
	tp->note = tp->pgnote;
	tp->notes = tp->pgnotes;
	return 1;
}

static void tg_apply_note_effects(xmlNodePtr tgnote, EOF_PRO_GUITAR_NOTE *np, unsigned bit)
{
	if(!tgnote || !np)
		return;
	if(tg_has_child(tgnote, "ghostNote"))
		np->ghost |= (unsigned char)bit;
	if(tg_has_child(tgnote, "palmMute"))
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_PALM_MUTE;
	if(tg_has_child(tgnote, "vibrato"))
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_VIBRATO;
	if(tg_has_child(tgnote, "accentuatedNote") || tg_has_child(tgnote, "heavyAccentuatedNote"))
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_ACCENT;
	if(tg_has_child(tgnote, "tapping"))
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_TAP;
	if(tg_has_child(tgnote, "slapping"))
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_SLAP;
	if(tg_has_child(tgnote, "popping"))
		np->flags |= EOF_PRO_GUITAR_NOTE_FLAG_POP;
	if(tg_has_child(tgnote, "letRing"))
		np->eflags |= EOF_PRO_GUITAR_NOTE_EFLAG_SUSTAIN;
}

static EOF_PRO_GUITAR_NOTE *tg_find_previous_string_note(EOF_PRO_GUITAR_TRACK *tp, unsigned char type, unsigned bit)
{
	unsigned long i;
	if(!tp || !tp->pgnotes)
		return NULL;
	for(i = tp->pgnotes; i > 0; i--)
	{
		EOF_PRO_GUITAR_NOTE *np = tp->pgnote[i - 1UL];
		if(np && (np->type == type) && (np->note & bit))
			return np;
	}
	return NULL;
}

static int tg_parse_voice(xmlNodePtr voice, EOF_PRO_GUITAR_TRACK *tp, unsigned char voice_index,
	const int *strings, unsigned string_count, int track_offset, int percussion,
	const TG_MEASURE_INFO *measure, size_t measure_count, long long beat_precise, unsigned long song_offset)
{
	xmlNodePtr n;
	long long duration_precise;
	double start_ms, end_ms;
	unsigned long pos;
	long length;

	duration_precise = tg_duration_precise(voice);
	if(duration_precise <= 0)
		duration_precise = TG_PRECISE_QUARTER / 4LL;
	start_ms = tg_precise_to_ms(measure, measure_count, beat_precise);
	end_ms = tg_precise_to_ms(measure, measure_count, beat_precise + duration_precise);
	pos = song_offset + (unsigned long)floor(start_ms + 0.5);
	length = (long)floor((end_ms - start_ms) + 0.5);
	if(length < 1) length = 1;

	if(percussion)
	{
		EOF_PRO_GUITAR_NOTE *np = NULL;
		unsigned slot = 0;
		for(n = voice->children; n; n = n->next)
		{
			int value, string_no, midi_note;
			if(!tg_node_is(n, "note"))
				continue;
			value = tg_attr_int(n, "value", 0);
			string_no = tg_attr_int(n, "string", 1);
			if((string_no < 1) || ((unsigned)string_no > string_count))
				continue;
			midi_note = track_offset + value + strings[string_no - 1];
			if(midi_note < 0) midi_note = 0;
			if(midi_note > 127) midi_note = 127;
			if(!np || (slot >= 6U))
			{
				np = tg_alloc_note(voice_index, pos, length);
				if(!np || !tg_track_add_note(tp, np))
				{
					free(np);
					return 0;
				}
				slot = 0;
			}
			np->note |= (unsigned char)(1U << slot);
			np->frets[slot] = (unsigned char)midi_note;
			tg_apply_note_effects(n, np, 1U << slot);
			slot++;
		}
		return 1;
	}
	else
	{
		EOF_PRO_GUITAR_NOTE *np = NULL;
		for(n = voice->children; n; n = n->next)
		{
			int fret, string_no, eof_string;
			unsigned bit;
			if(!tg_node_is(n, "note"))
				continue;
			fret = tg_attr_int(n, "value", 0);
			string_no = tg_attr_int(n, "string", 1);
			if((string_no < 1) || ((unsigned)string_no > string_count))
				continue;
			eof_string = (int)string_count - string_no;
			if((eof_string < 0) || (eof_string >= 6))
				continue;
			bit = 1U << eof_string;
			if(tg_attr_true(n, "tiedNote"))
			{
				EOF_PRO_GUITAR_NOTE *previous = tg_find_previous_string_note(tp, voice_index, bit);
				if(previous)
				{
					long tied_length = (long)((song_offset + (unsigned long)floor(end_ms + 0.5)) - previous->pos);
					if(tied_length > previous->length)
						previous->length = tied_length;
					continue;
				}
			}
			if(!np)
			{
				np = tg_alloc_note(voice_index, pos, length);
				if(!np)
					return 0;
			}
			np->note |= (unsigned char)bit;
			np->frets[eof_string] = (unsigned char)((fret < 0) ? 0 : ((fret > 127) ? 127 : fret));
			if(tg_has_child(n, "deadNote"))
				np->frets[eof_string] |= 0x80U;
			tg_apply_note_effects(n, np, bit);
		}
		if(np)
		{
			if(!tg_track_add_note(tp, np))
			{
				free(np);
				return 0;
			}
		}
		return 1;
	}
}

static int tg_parse_track(xmlNodePtr node, EOF_PRO_GUITAR_TRACK *tp, int percussion,
	const TG_MEASURE_INFO *measure, size_t measure_count, unsigned long song_offset)
{
	xmlNodePtr cur;
	int strings[32] = {0};
	unsigned string_count = 0, measure_index = 0;
	int track_offset = tg_child_int(node, "offset", 0);

	for(cur = node->children; cur; cur = cur->next)
	{
		if(tg_node_is(cur, "TGString") && (string_count < 32U))
			strings[string_count++] = tg_node_int(cur, 0);
	}
	if(!string_count)
	{
		strings[0] = percussion ? 0 : 64;
		string_count = 1;
	}
	if(percussion)
	{
		tp->numstrings = 6;
		tp->numfrets = 127;
	}
	else
	{
		unsigned s, supported = (string_count > 6U) ? 6U : string_count;
		tp->numstrings = (unsigned char)supported;
		for(s = 0; s < supported; s++)
			tp->tuning[s] = (char)strings[supported - 1U - s];
	}

	for(cur = node->children; cur; cur = cur->next)
	{
		xmlNodePtr beat;
		if(!tg_node_is(cur, "TGMeasure"))
			continue;
		for(beat = cur->children; beat; beat = beat->next)
		{
			xmlNodePtr voice;
			unsigned voice_index = 0;
			long long precise_start;
			if(!tg_node_is(beat, "TGBeat"))
				continue;
			precise_start = tg_node_ll(tg_child(beat, "preciseStart"),
				(measure_index < measure_count) ? measure[measure_index].precise_start : TG_PRECISE_QUARTER);
			for(voice = beat->children; voice; voice = voice->next)
			{
				if(!tg_node_is(voice, "voice"))
					continue;
				if(voice_index < 2U)
				{
					if(!tg_parse_voice(voice, tp, (unsigned char)voice_index, strings, string_count, track_offset,
						percussion, measure, measure_count, precise_start, song_offset))
						return 0;
				}
				voice_index++;
			}
		}
		measure_index++;
	}
	return 1;
}

static void tg_free_gp(struct eof_guitar_pro_struct *gp)
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
				for(j = 0; j < gp->track[i]->pgnotes; j++)
					free(gp->track[i]->pgnote[j]);
				for(j = 0; j < gp->track[i]->technotes; j++)
					free(gp->track[i]->technote[j]);
				free(gp->track[i]);
			}
		}
	}
	if(gp->names)
	{
		for(i = 0; i < gp->numtracks; i++)
			free(gp->names[i]);
	}
	free(gp->names);
	free(gp->track);
	free(gp->instrument_types);
	free(gp->measure);
	free(gp);
}

struct eof_guitar_pro_struct *eof_load_tg(const char *fn, char *undo_made)
{
	unsigned char *zip = NULL, *version = NULL, *xml = NULL;
	size_t zip_size = 0, version_size = 0, xml_size = 0;
	char error[256] = {0};
	xmlDocPtr doc = NULL;
	xmlNodePtr song, cur;
	TG_MEASURE_INFO *measure = NULL;
	size_t measure_count = 0, channel_count = 0;
	TG_CHANNEL *channels = NULL;
	struct eof_guitar_pro_struct *gp = NULL;
	unsigned long track_count = 0, track_index = 0;
	unsigned long song_offset;

	if(!fn || !eof_song)
		return NULL;
	(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tImporting TuxGuitar file \"%s\"", fn);
	eof_log(eof_log_string, 1);
	eof_log("eof_load_tg() entered", 1);

	if(!tg_read_container(fn, &zip, &zip_size, error, sizeof(error)))
		goto fail;
	if(!tg_zip_extract(zip, zip_size, TG_VERSION_PATH, &version, &version_size, error, sizeof(error)))
		goto fail;
	if(strncmp((const char *)version, TG_VERSION_PREFIX, strlen(TG_VERSION_PREFIX)) ||
	   (((const char *)version)[strlen(TG_VERSION_PREFIX)] != '2'))
	{
		tg_set_error(error, sizeof(error), "Unsupported TuxGuitar format version. This importer supports native TuxGuitar 2.x .tg files.");
		goto fail;
	}
	if(!tg_zip_extract(zip, zip_size, TG_CONTENT_PATH, &xml, &xml_size, error, sizeof(error)))
		goto fail;

	doc = xmlReadMemory((const char *)xml, (int)xml_size, fn, NULL,
		XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if(!doc)
	{
		tg_set_error(error, sizeof(error), "Unable to parse TuxGuitar content.xml");
		goto fail;
	}
	song = tg_find_song(doc);
	if(!song)
	{
		tg_set_error(error, sizeof(error), "content.xml is not a TuxGuitar 2.x song");
		goto fail;
	}
	if(!tg_parse_measures(song, &measure, &measure_count))
	{
		tg_set_error(error, sizeof(error), "TuxGuitar song has no readable measure headers");
		goto fail;
	}
	channels = tg_parse_channels(song, &channel_count);
	for(cur = song->children; cur; cur = cur->next)
		if(tg_node_is(cur, "TGTrack"))
			track_count++;
	if(!track_count)
	{
		tg_set_error(error, sizeof(error), "TuxGuitar song contains no tracks");
		goto fail;
	}

	gp = calloc(1, sizeof(*gp));
	if(!gp)
		goto oom;
	gp->numtracks = track_count;
	gp->fileversion = 200;
	gp->names = calloc(track_count, sizeof(*gp->names));
	gp->instrument_types = calloc(track_count, sizeof(*gp->instrument_types));
	gp->track = calloc(track_count, sizeof(*gp->track));
	if(!gp->names || !gp->instrument_types || !gp->track)
		goto oom;

	song_offset = eof_song->beats ? eof_song->beat[0]->pos : 0UL;
	for(cur = song->children; cur; cur = cur->next)
	{
		EOF_PRO_GUITAR_TRACK *tp;
		char *name;
		int channel_id, maxfret, percussion = 0;
		const TG_CHANNEL *channel;
		if(!tg_node_is(cur, "TGTrack"))
			continue;
		tp = calloc(1, sizeof(*tp));
		if(!tp)
			goto oom;
		tp->note = tp->pgnote;
		tp->parent = NULL;
		maxfret = tg_attr_int(cur, "maxFret", 24);
		if(maxfret < 1) maxfret = 24;
		if(maxfret > 127) maxfret = 127;
		tp->numfrets = (unsigned char)maxfret;
		gp->track[track_index] = tp;

		name = tg_node_text(tg_child(cur, "name"));
		if(!name || !*name)
		{
			char fallback[64];
			free(name);
			(void)snprintf(fallback, sizeof(fallback), "TuxGuitar track %lu", track_index + 1UL);
			name = tg_strdup(fallback);
		}
		gp->names[track_index] = name;
		if(!name)
			goto oom;

		channel_id = tg_child_int(cur, "channelId", -1);
		channel = tg_find_channel(channels, channel_count, channel_id);
		if(channel && (channel->bank == 128))
			percussion = 1;
		if(percussion)
			gp->instrument_types[track_index] = 3;
		else if(channel && (channel->program >= 32) && (channel->program <= 39))
		{
			gp->instrument_types[track_index] = 2;
			tp->arrangement = EOF_BASS_ARRANGEMENT;
		}
		else
			gp->instrument_types[track_index] = 1;

		if(!tg_parse_track(cur, tp, percussion, measure, measure_count, song_offset))
			goto oom;
		track_index++;
	}

	if(!tg_apply_tempo_map(measure, measure_count, undo_made))
	{
		tg_set_error(error, sizeof(error), "Unable to apply the TuxGuitar tempo/time-signature map");
		goto fail;
	}


	free(channels);
	free(measure);
	xmlFreeDoc(doc);
	free(xml);
	free(version);
	free(zip);
	eof_log("\tTuxGuitar 2.x parsing complete", 1);
	return gp;

oom:
	tg_set_error(error, sizeof(error), "Out of memory while importing TuxGuitar song");
fail:
	if(error[0])
	{
		(void)snprintf(eof_log_string, sizeof(eof_log_string) - 1, "\tTuxGuitar import failed: %s", error);
		eof_log(eof_log_string, 1);
		allegro_message("TuxGuitar import failed:\n%s", error);
	}
	tg_free_gp(gp);
	free(channels);
	free(measure);
	if(doc) xmlFreeDoc(doc);
	free(xml);
	free(version);
	free(zip);
	return NULL;
}

#else

#include "tg_import.h"
struct eof_guitar_pro_struct *eof_load_tg(const char *fn, char *undo_made)
{
	(void)fn;
	(void)undo_made;
	return NULL;
}

#endif
