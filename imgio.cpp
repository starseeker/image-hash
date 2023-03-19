// MIT License
//
// Copyright (c) 2021 Samuel Bear Powell
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// https://github.com/s-bear/image-hash

#include "PImgHash.h"
#include "imgio.h"

#include "png.h"

#include <fstream>
#include <cstdio>
#include <bitset>
#include <algorithm>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace imghash
{

bool test_ppm(FILE* file)
{
    unsigned char magic[2] = { 0 };
    auto off = ftell(file);
    fread(magic, sizeof(unsigned char), 2, file);
    fseek(file, off, SEEK_SET);
    return (magic[0] == 'P') && (magic[1] == '6');
}

Image<float> load_ppm(FILE* file, Preprocess& prep, bool empty_error)
{

    // 1. Magic number
    // 2. Whitespace
    // 3. Width, ASCII decimal
    // 4. Whitespace
    // 5. Height, ASCII decimal
    // 6. Whitespace
    // 7. Maxval, ASCII decimal
    // 8. A single whitespace character
    // 9. Raster (width x height x 3) bytes, x2 if maxval > 255, MSB first
    // At any point before 8, # begins a comment, which persists until the next newline or carriage return

    const size_t maxsize = 0x40000000; // 1 GB
    const size_t bufsize = 256;
    char buffer[bufsize] = { 0 };

    auto parse_space = [&](int c) {
	bool comment = ((char)c == '#');
	while (isspace(c) || (comment && c != EOF)) {
	    c = fgetc(file);
	    if (comment) {
		if ((char)c == '\r' || (char)c == '\n') comment = false;
	    } else {
		if ((char)c == '#') comment = true;
	    }
	}
	if (c == EOF) {
	    throw std::runtime_error("PPM: Unexpected EOF");
	}
	return c;
    };
    auto parse_size = [&](int c, size_t& x) {
	size_t i = 0;
	while (isdigit(c)) {
	    buffer[i++] = (char)c;
	    if (i >= bufsize - 1) {
		throw std::runtime_error("PPM: Buffer overflow");
	    }
	    c = fgetc(file);
	}
	if (c == EOF) {
	    throw std::runtime_error("PPM: Unexpected EOF");
	}
	buffer[i] = 0;
	x = atoll(buffer);
	return c;
    };

    //1. Magic number
    if (fread(buffer, sizeof(char), 2, file) == 0) {
	//empty file / end of stream
	if (empty_error) throw std::runtime_error("PPM: Empty file");
	else return Image<float>();
    }

    if (buffer[0] != 'P' || buffer[1] != '6') {
	throw std::runtime_error(std::string("PPM: Invalid file (") + buffer + ")");
    }

    // 2. Whitespace or comment
    int c = fgetc(file);
    c = parse_space(c);

    // 3. Width, ASCII decimal
    size_t width = 0;
    c = parse_size(c, width);

    // 4. Whitespace
    c = parse_space(c);

    // 5. Height, ASCII decimal
    size_t height = 0;
    c = parse_size(c, height);

    // 6. Whitespace
    c = parse_space(c);

    // 7. Maxval, ASCII decimal
    size_t maxval = 0;
    c = parse_size(c, maxval);

    //any final comment
    bool comment = ((char)c == '#');
    while (comment && c != EOF) {
	c = fgetc(file);
	if (c == '\r' || c == '\n') comment = false;
    }
    if (c == EOF) {
	throw std::runtime_error("PPM: Unexpected EOF");
    }
    // 8. A single whitespace character
    if (!isspace(c)) {
	throw std::runtime_error("PPM: No whitespace after maxval");
    }

    //check dimensions
    size_t rowsize = width * 3;
    size_t size = rowsize * height; //TODO: overflow?
    bool use_short = maxval > 0xFF;
    if (use_short) size *= 2;
    if (maxval > 0xFFFF) {
	throw std::runtime_error("PPM: Invalid maxval");
    }
    if (size > maxsize) {
	throw std::runtime_error("PPM: Size overflow");
    }

    // 9. Raster (width x height x 3) bytes, x2 if maxval > 255, MSB first
    prep.start(height, width, 3);
    if (use_short) {
	std::vector<uint16_t> row(rowsize, 0);
	do {
	    size_t i;
	    for (i = 0; i < rowsize; ++i) {
		if (fread(buffer, 1, 2, file) < 2) break;
		row[i] = (buffer[0] << 8) | (buffer[1]); //deal with endianness
	    }
	    if (i < rowsize) {
		throw std::runtime_error("PPM: Not enough data");
	    }
	} while (prep.add_row(row.data()));
    } else {
	std::vector<uint8_t> row(rowsize, 0);
	do {
	    if (fread(row.data(), 1, rowsize, file) < rowsize) {
		throw std::runtime_error("PPM: Not enough data");
	    }
	} while (prep.add_row(row.data()));
    }
    return prep.stop();
}

#ifdef USE_PNG
bool test_png(FILE* file)
{
    png_byte header[8] = { 0 };
    auto off = ftell(file);
    fread(header, sizeof(png_byte), 8, file);
    fseek(file, off, SEEK_SET);
    return png_sig_cmp(header, 0, 8) == 0;
}

namespace
{
void my_error_fn(png_structp png_ptr, png_const_charp error_msg)
{
    std::string* msg = (std::string*)png_get_error_ptr(png_ptr);
    *msg = error_msg;
    longjmp(png_jmpbuf(png_ptr), 1);
}
void my_warning_fn(png_structp png_ptr, png_const_charp warning_msg)
{
    return; //ignore warnings
}
}

Image<float> load_png(FILE* file, Preprocess& prep)
{
    std::string error_message;
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
			  &error_message, my_error_fn, my_warning_fn);
    if (!png_ptr) {
	throw std::runtime_error("PNG: Error creating read struct");
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
	png_destroy_read_struct(&png_ptr, nullptr, nullptr);
	throw std::runtime_error("PNG: Error creating info struct");
    }
    png_infop end_info = png_create_info_struct(png_ptr);
    if (!info_ptr) {
	png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
	throw std::runtime_error("PNG: Error creating info struct");
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
	png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
	throw std::runtime_error("PNG: Error");
    }

    png_init_io(png_ptr, file);
    //ignore all unkown chunks
    png_set_keep_unknown_chunks(png_ptr, PNG_HANDLE_CHUNK_NEVER, nullptr, 0);

    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = png_get_image_width(png_ptr, info_ptr);
    png_uint_32 height = png_get_image_height(png_ptr, info_ptr);
    png_byte channels = png_get_channels(png_ptr, info_ptr);
    int color_type = png_get_color_type(png_ptr, info_ptr);
    int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    int interlace = png_get_interlace_type(png_ptr, info_ptr);
    //we want an RGB image with no alpha
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
	png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY
	|| color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
	png_set_gray_to_rgb(png_ptr);

	if (bit_depth < 8) {
	    png_set_expand_gray_1_2_4_to_8(png_ptr);
	}
    }
    if (bit_depth == 16) {
	png_set_strip_16(png_ptr);
    }

    png_color_16 bg = { 0 };
    png_color_16p bg_ptr;
    if (png_get_bKGD(png_ptr, info_ptr, &bg_ptr)) {
	png_set_background(png_ptr, bg_ptr, PNG_BACKGROUND_GAMMA_FILE, 1, 1.0);
    } else {
	png_set_background(png_ptr, &bg, PNG_BACKGROUND_GAMMA_FILE, 0, 1.0);
    }
    /*
       double gamma;
       if (!png_get_gAMA(png_ptr, info_ptr, &gamma)) gamma = 0.4545;
       png_set_gamma(png_ptr, 0.4545, gamma);
       */

    png_read_update_info(png_ptr, info_ptr);

    channels = png_get_channels(png_ptr, info_ptr);
    size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    if (interlace == PNG_INTERLACE_NONE) {
	//we can read row-by-row
	std::vector<uint8_t> row(rowbytes, 0);
	prep.start(height, width, channels);
	do {
	    png_read_row(png_ptr, row.data(), nullptr);
	} while (prep.add_row(row.data()));
	return prep.stop();
    } else {
	//interlaced.. let libpng handle it
	Image<uint8_t> img(height, width, channels, rowbytes*height, rowbytes);
	std::vector<uint8_t*> rows;
	rows.reserve(height);
	for (size_t y = 0; y < height; ++y) rows.push_back(img.data->data() + img.index(y,0,0));
	png_read_image(png_ptr, rows.data());

	return prep.apply(img);
    }
}

Image<float> load(const std::string& fname, Preprocess& prep)
{
    FILE* file = fopen(fname.c_str(), "rb");
    if (file == nullptr) {
	throw std::runtime_error("Failed to open file");
    }
    try {
	Image<float> img;
	if (test_ppm(file)) {
	    img = load_ppm(file, prep);
	} else if (test_png(file)) {
	    img = load_png(file, prep);
	} else {
	    throw std::runtime_error("Unsupported file format");
	}
	fclose(file);
	return img;
    } catch (std::exception& e) {
	fclose(file);
	throw e;
    }
}

#endif

}




// Local Variables:
// tab-width: 8
// mode: C++
// c-basic-offset: 4
// indent-tabs-mode: t
// c-file-style: "stroustrup"
// End:
// ex: shiftwidth=4 tabstop=8
