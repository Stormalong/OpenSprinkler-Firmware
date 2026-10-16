#include "sitemap.h"

#include "handler_context.h"
#include "handlers.h"

#include "storage/files.h"
#include "storage/maintenance.h"

#include <cstdlib>
#include <cstring>
#include <new>

/*
 * Site map storage for the apps: station pins on a background image.
 *
 * smap.dat layout (little endian):
 *   uint32 revision, uint32 image size, uint16 pin count,
 *   then per pin: uint8 station index, uint16 x, uint16 y (0 to SITEMAP_COORD_MAX across the image)
 * The JPEG image is stored separately in smapimg.dat and uploaded in base64 chunks through smapimg.tmp;
 * base64 keeps the upload intact when it is relayed as text through the OpenThings cloud.
 * Head runs and drip lines are stored as app-defined ASCII text in smapdrw.dat, uploaded in plain text chunks
 * through smapdrw.tmp; the firmware only checks that it is printable text.
 */

namespace {

constexpr uint16_t SITEMAP_COORD_MAX = 10000;
constexpr uint32_t SITEMAP_HEADER_SIZE = 10;
constexpr uint32_t SITEMAP_PIN_SIZE = 5;

struct SiteMapHeader {
	uint32_t revision;
	uint32_t image_size;
	uint16_t pin_count;
};

int64_t cached_revision = -1;

void put_u16(uint8_t* bytes, uint16_t value) {
	bytes[0] = value & 0xFF;
	bytes[1] = value >> 8;
}

void put_u32(uint8_t* bytes, uint32_t value) {
	for (unsigned char index = 0; index < 4; index++) bytes[index] = (value >> (index * 8)) & 0xFF;
}

uint16_t get_u16(const uint8_t* bytes) {
	return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

uint32_t get_u32(const uint8_t* bytes) {
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

// A missing or unreadable file is an empty map with revision 0.
SiteMapHeader read_header() {
	SiteMapHeader header = {0, 0, 0};
	uint8_t bytes[SITEMAP_HEADER_SIZE];
	if (file_exists(SITEMAP_FILENAME) && file_read_block(SITEMAP_FILENAME, bytes, 0, SITEMAP_HEADER_SIZE)) {
		header.revision = get_u32(bytes);
		header.image_size = get_u32(bytes + 4);
		header.pin_count = get_u16(bytes + 8);
	}
	return header;
}

void encode_header(const SiteMapHeader& header, uint8_t* bytes) {
	put_u32(bytes, header.revision);
	put_u32(bytes + 4, header.image_size);
	put_u16(bytes + 8, header.pin_count);
}

// Rewrites the whole file: the header followed by count pins.
bool write_map(const SiteMapHeader& header, const uint8_t* pins) {
	uint8_t bytes[SITEMAP_HEADER_SIZE];
	encode_header(header, bytes);
	os_file_type file = file_open(SITEMAP_FILENAME, FileOpenMode::WriteTruncate);
	if (!file) return false;
	uint32_t pins_length = (uint32_t)header.pin_count * SITEMAP_PIN_SIZE;
	bool ok = file_write(file, bytes, SITEMAP_HEADER_SIZE) == (int)SITEMAP_HEADER_SIZE &&
		(pins_length == 0 || file_write(file, pins, pins_length) == (int)pins_length);
	file_close(file);
	if (ok) cached_revision = header.revision;
	return ok;
}

// Updates the header while keeping the stored pins.
bool write_header(const SiteMapHeader& header) {
	if (!file_exists(SITEMAP_FILENAME)) {
		SiteMapHeader empty = header;
		empty.pin_count = 0;
		return write_map(empty, nullptr);
	}
	uint8_t bytes[SITEMAP_HEADER_SIZE];
	encode_header(header, bytes);
	if (!file_write_block(SITEMAP_FILENAME, bytes, 0, SITEMAP_HEADER_SIZE)) return false;
	cached_revision = header.revision;
	return true;
}

uint32_t current_file_size(const char* filename) {
	if (!file_exists(filename)) return 0;
	os_file_type file = file_open(filename, FileOpenMode::Read);
	if (!file) return 0;
	uint32_t size = file_size(file);
	file_close(file);
	return size;
}

bool parse_uint(const char* text, uint32_t max_value, uint32_t& value) {
	if (!text || !text[0]) return false;
	char* end = nullptr;
	unsigned long parsed = strtoul(text, &end, 10);
	if (*end || parsed > max_value) return false;
	value = (uint32_t)parsed;
	return true;
}

int base64_value(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

// Decodes base64 in place; the output is always shorter than the input.
bool decode_base64(char* data, size_t length, uint32_t& decoded) {
	if (length % 4) return false;
	decoded = 0;
	for (size_t position = 0; position < length; position += 4) {
		int values[4];
		unsigned char padding = 0;
		for (unsigned char index = 0; index < 4; index++) {
			char c = data[position + index];
			bool last_group = position + 4 == length;
			if (c == '=' && last_group && index >= 2) {
				values[index] = 0;
				padding++;
				continue;
			}
			values[index] = padding ? -1 : base64_value(c);
			if (values[index] < 0) return false;
		}
		uint32_t bits = ((uint32_t)values[0] << 18) | ((uint32_t)values[1] << 12) | ((uint32_t)values[2] << 6) | (uint32_t)values[3];
		data[decoded++] = (char)((bits >> 16) & 0xFF);
		if (padding < 2) data[decoded++] = (char)((bits >> 8) & 0xFF);
		if (padding < 1) data[decoded++] = (char)(bits & 0xFF);
	}
	return true;
}

bool is_drawing_text(const char* data, uint32_t length) {
	for (uint32_t index = 0; index < length; index++) {
		char c = data[index];
		if (c != '\n' && (c < 0x20 || c > 0x7E)) return false;
	}
	return true;
}

// Writes one chunk of a chunked upload to temp_file, then moves it to file once total bytes have arrived.
// Returns HTML_OK when more chunks are expected, HTML_SUCCESS when the upload completed, or an error code.
unsigned char write_upload_chunk(const char* temp_file, const char* file, const char* data, uint32_t length,
	uint32_t offset, uint32_t total) {
	if (!total || offset + length > total) return HTML_DATA_OUTOFBOUND;

	if (offset == 0) {
		remove_file(temp_file);
#if defined(ARDUINO)
		EmbeddedStorageUsage usage = get_embedded_storage_usage();
		if (usage.valid && usage.free_bytes < total + embedded_storage_reserve_bytes(usage)) {
			return HTML_DATA_OUTOFBOUND;
		}
#endif
	} else if (current_file_size(temp_file) != offset) {
		return HTML_DATA_OUTOFBOUND;
	}

	if (!file_write_block(temp_file, data, offset, length)) {
		remove_file(temp_file);
		return HTML_UPLOAD_FAILED;
	}
	if (offset + length < total) return HTML_OK;
	if (!rename_file(temp_file, file)) {
		remove_file(temp_file);
		return HTML_INTERNAL_ERROR;
	}
	return HTML_SUCCESS;
}

bool bump_revision() {
	SiteMapHeader header = read_header();
	header.revision++;
	return write_header(header);
}

// Streams a stored file as the response body.
#if defined(ARDUINO)
using flash_text = const __FlashStringHelper*;
#else
using flash_text = const char*;
#endif

void send_file(OTF::Response& res, const char* filename, uint32_t size, flash_text content_type) {
	os_file_type file = file_open(filename, FileOpenMode::Read);
	if (!file) return;

	res.writeStatus(200, F("OK"));
	res.writeHeader(F("Content-Type"), content_type);
	res.writeHeader(F("Content-Length"), (int)size);
	res.writeHeader(F("Access-Control-Allow-Origin"), F("*"));
	res.writeHeader(F("Cache-Control"), F("max-age=0, no-cache, no-store, must-revalidate"));
	res.writeHeader(F("Connection"), F("close"));
	res.writeBodyData("", 0);

	uint32_t remaining = size;
	while (remaining > 0 && res.isValid()) {
		uint32_t chunk = remaining < TMP_BUFFER_SIZE ? remaining : TMP_BUFFER_SIZE;
		if (file_read(file, tmp_buffer, chunk) != (int)chunk) break;
		res.writeBodyData(tmp_buffer, chunk);
		remaining -= chunk;
	}
	file_close(file);
}

// Parses "sid,x,y;sid,x,y;..." into packed pins. Returns an HTML_* result code.
unsigned char parse_pins(const char* source, size_t length, uint8_t* pins, uint16_t& count) {
	uint8_t seen[(MAX_NUM_STATIONS + 7) / 8] = {0};
	count = 0;
	size_t position = 0;
	while (position < length) {
		uint32_t values[3];
		for (unsigned char field = 0; field < 3; field++) {
			char number[8];
			size_t digits = 0;
			while (position < length && source[position] >= '0' && source[position] <= '9') {
				if (digits >= sizeof(number) - 1) return HTML_DATA_FORMATERROR;
				number[digits++] = source[position++];
			}
			number[digits] = 0;
			char expected = field < 2 ? ',' : ';';
			bool at_end = position >= length;
			if (!digits || (!at_end && source[position] != expected) || (at_end && field < 2)) {
				return HTML_DATA_FORMATERROR;
			}
			if (!at_end) position++;
			values[field] = strtoul(number, nullptr, 10);
		}

		uint32_t sid = values[0];
		if (sid >= os.nstations || values[1] > SITEMAP_COORD_MAX || values[2] > SITEMAP_COORD_MAX) {
			return HTML_DATA_OUTOFBOUND;
		}
		if (seen[sid >> 3] & (1 << (sid & 7))) return HTML_DATA_FORMATERROR;
		seen[sid >> 3] |= 1 << (sid & 7);

		uint8_t* pin = pins + (uint32_t)count * SITEMAP_PIN_SIZE;
		pin[0] = (uint8_t)sid;
		put_u16(pin + 1, (uint16_t)values[1]);
		put_u16(pin + 3, (uint16_t)values[2]);
		count++;
	}
	return HTML_SUCCESS;
}

} // namespace

uint32_t sitemap_revision() {
	if (cached_revision < 0) cached_revision = read_header().revision;
	return (uint32_t)cached_revision;
}

/** Output the site map pins
 * Command: /jm?pw=xxx
 * Returns {"rev":N,"img":image bytes,"drw":drawings bytes,"pins":[[sid,x,y],...]} with x/y from 0 to 10000.
 * Pins for stations that no longer exist are left out.
 */
void server_json_sitemap(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;
	SiteMapHeader header = read_header();

	begin_response(res);
	print_header(OTF_PARAMS);
	bfill.emit_p(PSTR("{\"rev\":$L,\"img\":$L,\"drw\":$L,\"pins\":["), header.revision, header.image_size,
		current_file_size(SITEMAP_DRAWINGS_FILENAME));

	bool comma = false;
	uint8_t pin[SITEMAP_PIN_SIZE];
	for (uint16_t index = 0; index < header.pin_count; index++) {
		if (!file_read_block(SITEMAP_FILENAME, pin, SITEMAP_HEADER_SIZE + (uint32_t)index * SITEMAP_PIN_SIZE,
			SITEMAP_PIN_SIZE)) break;
		if (pin[0] >= os.nstations) continue;
		if (comma) bfill.emit_p(PSTR(","));
		comma = true;
		bfill.emit_p(PSTR("[$D,$D,$D]"), pin[0], get_u16(pin + 1), get_u16(pin + 3));
	}
	bfill.emit_p(PSTR("]}"));
	handle_return(HTML_OK);
}

/** Replace the site map pins
 * Command: /cmp?pw=xxx with the pins as the request body: sid,x,y;sid,x,y;...
 * For small maps the pins may be sent as the query parameter p instead; an empty p clears all pins.
 */
void server_change_sitemap_pins(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;

	const char* source = req.getBody();
	size_t length = req.getBodyLength();
	if (!length) {
		source = req.getQueryParameter("p");
		if (!source) handle_return(HTML_DATA_MISSING);
		length = strlen(source);
	}

	uint8_t* pins = new (std::nothrow) uint8_t[MAX_NUM_STATIONS * SITEMAP_PIN_SIZE];
	if (!pins) handle_return(HTML_INTERNAL_ERROR);
	uint16_t count = 0;
	unsigned char result = parse_pins(source, length, pins, count);
	if (result == HTML_SUCCESS) {
		SiteMapHeader header = read_header();
		header.revision++;
		header.pin_count = count;
		if (!write_map(header, pins)) result = HTML_INTERNAL_ERROR;
	}
	delete[] pins;
	handle_return(result);
}

/** Upload one chunk of the site map image
 * Command: /um?pw=xxx&off=offset&tot=total size, with the chunk base64 encoded as the request body
 * off and tot count decoded bytes. Chunks must arrive in order; off=0 starts a new upload.
 * The image replaces the current one once the last chunk arrives and must be a JPEG.
 */
void server_upload_sitemap_image(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;

	uint32_t offset = 0;
	uint32_t total = 0;
	if (!parse_uint(req.getQueryParameter("off"), SITEMAP_MAX_IMAGE_BYTES, offset) ||
		!parse_uint(req.getQueryParameter("tot"), SITEMAP_MAX_IMAGE_BYTES, total)) {
		handle_return(HTML_DATA_OUTOFBOUND);
	}

	// Also rejects bodiless requests such as CORS preflights
	char* body = req.getBody();
	uint32_t length = 0;
	if (!body || !req.getBodyLength()) handle_return(HTML_DATA_MISSING);
	if (!decode_base64(body, req.getBodyLength(), length) || !length) handle_return(HTML_DATA_FORMATERROR);

	if (!total || offset + length > total) handle_return(HTML_DATA_OUTOFBOUND);

	// The JPEG signature is checked on the first chunk, before anything is written
	if (offset == 0 && (length < 2 || (uint8_t)body[0] != 0xFF || (uint8_t)body[1] != 0xD8)) {
		handle_return(HTML_DATA_FORMATERROR);
	}
	unsigned char result = write_upload_chunk(SITEMAP_IMAGE_TEMP_FILENAME, SITEMAP_IMAGE_FILENAME, body, length,
		offset, total);
	if (result == HTML_OK) handle_return(HTML_SUCCESS);
	if (result != HTML_SUCCESS) handle_return(result);

	SiteMapHeader header = read_header();
	header.revision++;
	header.image_size = total;
	handle_return(write_header(header) ? HTML_SUCCESS : HTML_INTERNAL_ERROR);
}

/** Download the site map image
 * Command: /jmi?pw=xxx
 * Returns the JPEG, or result 0x10 when no image is stored.
 */
void server_json_sitemap_image(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;

	SiteMapHeader header = read_header();
	uint32_t size = current_file_size(SITEMAP_IMAGE_FILENAME);
	if (!header.image_size || size != header.image_size) handle_return(HTML_DATA_MISSING);
	send_file(res, SITEMAP_IMAGE_FILENAME, size, F("image/jpeg"));
}

/** Upload one chunk of the site map drawings (head runs and drip lines)
 * Command: /umd?pw=xxx&off=offset&tot=total size, with the chunk as a plain text request body
 * Chunks must arrive in order; off=0 starts a new upload, and tot=0 with an empty body removes the drawings.
 * The text format belongs to the apps; it must be printable ASCII with newlines.
 */
void server_upload_sitemap_drawings(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;

	uint32_t offset = 0;
	uint32_t total = 0;
	if (!parse_uint(req.getQueryParameter("off"), SITEMAP_MAX_DRAWING_BYTES, offset) ||
		!parse_uint(req.getQueryParameter("tot"), SITEMAP_MAX_DRAWING_BYTES, total)) {
		handle_return(HTML_DATA_OUTOFBOUND);
	}

	const char* body = req.getBody();
	uint32_t length = (uint32_t)req.getBodyLength();
	if (total == 0 && offset == 0) {
		remove_file(SITEMAP_DRAWINGS_TEMP_FILENAME);
		if (!remove_file(SITEMAP_DRAWINGS_FILENAME)) handle_return(HTML_INTERNAL_ERROR);
		handle_return(bump_revision() ? HTML_SUCCESS : HTML_INTERNAL_ERROR);
	}

	// Also rejects bodiless requests such as CORS preflights
	if (!body || !length) handle_return(HTML_DATA_MISSING);
	if (!is_drawing_text(body, length)) handle_return(HTML_DATA_FORMATERROR);
	unsigned char result = write_upload_chunk(SITEMAP_DRAWINGS_TEMP_FILENAME, SITEMAP_DRAWINGS_FILENAME, body, length,
		offset, total);
	if (result == HTML_OK) handle_return(HTML_SUCCESS);
	if (result != HTML_SUCCESS) handle_return(result);
	handle_return(bump_revision() ? HTML_SUCCESS : HTML_INTERNAL_ERROR);
}

/** Download the site map drawings
 * Command: /jmd?pw=xxx
 * Returns the drawings text, or result 0x10 when none are stored.
 */
void server_json_sitemap_drawings(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;
	uint32_t size = current_file_size(SITEMAP_DRAWINGS_FILENAME);
	if (!size) handle_return(HTML_DATA_MISSING);
	send_file(res, SITEMAP_DRAWINGS_FILENAME, size, F("text/plain"));
}

/** Delete the site map pins, drawings and image
 * Command: /dm?pw=xxx
 */
void server_delete_sitemap(OTF_PARAMS_DEF) {
	if (!process_password(OTF_PARAMS)) return;
	remove_file(SITEMAP_IMAGE_TEMP_FILENAME);
	remove_file(SITEMAP_IMAGE_FILENAME);
	remove_file(SITEMAP_DRAWINGS_TEMP_FILENAME);
	remove_file(SITEMAP_DRAWINGS_FILENAME);
	SiteMapHeader header = {read_header().revision + 1, 0, 0};
	handle_return(write_map(header, nullptr) ? HTML_SUCCESS : HTML_INTERNAL_ERROR);
}
