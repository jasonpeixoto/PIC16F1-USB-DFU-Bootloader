/*
    command-line tool to convert PIC16F1454 Intel Hex to DFU binary
    Copyright (C) 2015 Peter Lawrence

	This was written to generate binaries to use with this bootloader:
  	https://github.com/majbthrd/PIC16F1-USB-DFU-Bootloader

    Permission is hereby granted, free of charge, to any person obtaining a 
    copy of this software and associated documentation files (the "Software"), 
    to deal in the Software without restriction, including without limitation 
    the rights to use, copy, modify, merge, publish, distribute, sublicense, 
    and/or sell copies of the Software, and to permit persons to whom the 
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in 
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL 
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
    DEALINGS IN THE SOFTWARE.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define PM_SIZE_IN_BYTES		 16384
#define	CODE_OFFSET_ADDRESS		 0x200
#define	HIGH_ENDURANCE_ADDRESS	0x1F80
#define DFU_SUFFIX				    16
#define USB_PRODUCT_ID			0x0001
#define USB_VENDOR_ID			0x1234

static unsigned readhex(const char *text, unsigned digits);
static unsigned calc_modified_crc14(unsigned data, unsigned crc);
static unsigned crc32_calc(unsigned crc, unsigned char *buffer, unsigned length);

int main(int argc, char *argv[])
{
	FILE *input, *output;
	char line[256];
	unsigned address, upper_address;
	unsigned count, next_address, crc;
	const char *ptr;
	struct
	{
		unsigned out_of_bounds:1;
		unsigned crc_overlap:1;
	} flags;
	unsigned char *image, *suffix;

	if (argc < 3)
	{
		fprintf(stderr, "%s <input_ihex> <output_dfu>\n", argv[0]);
		return -1;
	}

	image = (unsigned char *)malloc(PM_SIZE_IN_BYTES + DFU_SUFFIX);

	if (NULL == image)
	{
		fprintf(stderr, "ERROR: unable to allocate memory\n");
		return -1;
	}

	input = fopen(argv[1], "rb");

	if (NULL == input)
	{
		fprintf(stderr, "ERROR: unable to open input file %s\n", argv[1]);
		return -1;
	}

	output = fopen(argv[2], "wb");

	if (NULL == output)
	{
		fprintf(stderr, "ERROR: unable to open output file %s\n", argv[2]);
		return -1;
	}

	for (address = 0; address < PM_SIZE_IN_BYTES; address += 2)
	{
		image[address + 0] = 0xFF;
		image[address + 1] = 0x3F;
	}

	memset(&flags, 0, sizeof(flags));
	upper_address = 0;

	while (!feof(input))
	{
		if (fgets(line, sizeof(line), input))
		{
			if (':' == line[0])
			{
				count = readhex(line + 1, 2);
				address = readhex(line + 3, 4);
				if (0 == strncmp(line + 7, "00", 2) && (0 == upper_address)) /* data record */
				{
					ptr = line + 9;
					while (count--)
					{
						if ( (address < 0x400) || (address >= 0x8000) )
							flags.out_of_bounds = 1;
						else
							image[address] = readhex(ptr, 2);
						address++; ptr += 2;
					}
				}
				else if (0 == strncmp(line + 7, "04", 2)) /* encoding of upper 16-bits (non-zero indicates not part of program memory) */
				{
					upper_address = readhex(line + 9, 4);
				}
				else if (0 == strncmp(line + 7, "01", 2)) /* end of file record */
				{
					break;
				}
			}
		}
	}

	fclose(input);

	if (flags.out_of_bounds)
	{
		fprintf(stderr, "ERROR: supplied input file is faulty and used out-of-bounds addresses\n");
		goto skip_write;
	}

	address = CODE_OFFSET_ADDRESS << 1; crc = 0;
	for (;;)
	{
		next_address = address + 2;
		if ((HIGH_ENDURANCE_ADDRESS << 1) == next_address)
		{
			flags.crc_overlap = (0xFF != image[address + 0]) || (0x3F != image[address + 1]);
			image[address + 0] = (crc & 0x00FF);
			image[address + 1] = (crc & 0xFF00) >> 8;
			break;
		}
		else
		{
			crc = calc_modified_crc14((unsigned)image[address + 0] + ((unsigned)image[address + 1] << 8), crc);
			address = next_address;
		}
	}

	if (flags.crc_overlap)
	{
		fprintf(stderr, "ERROR: CRC address was occupied; app is in conflict with bootloader\n");
		goto skip_write;
	}

	suffix = image + PM_SIZE_IN_BYTES;
	count = 0;
	suffix[count++] = 0xFF;								// bcdDevice
	suffix[count++] = 0xFF;
	suffix[count++] = (USB_PRODUCT_ID & 0x00FF) >> 0;	// idProduct
	suffix[count++] = (USB_PRODUCT_ID & 0xFF00) >> 8;
	suffix[count++] = (USB_VENDOR_ID & 0x00FF) >> 0;	// idVendor
	suffix[count++] = (USB_VENDOR_ID & 0xFF00) >> 8;
	suffix[count++] = 0x00;								// bcdDFU
	suffix[count++] = 0x01;
	suffix[count++] = 'U';								// ucDfuSignature
	suffix[count++] = 'F';
	suffix[count++] = 'D';
	suffix[count++] = DFU_SUFFIX;						// bLength

	crc = crc32_calc(0xFFFFFFFF, image, PM_SIZE_IN_BYTES + count);

	suffix[count++] = (crc & 0x000000FF) >> 0;			// dwCRC
	suffix[count++] = (crc & 0x0000FF00) >> 8;
	suffix[count++] = (crc & 0x00FF0000) >> 16;
	suffix[count++] = (crc & 0xFF000000) >> 24;

	assert(DFU_SUFFIX == count);

	fwrite(image, 1, PM_SIZE_IN_BYTES + count, output);
	fclose(output);

skip_write:
	free(image);

	return 0;
}

static unsigned readhex(const char *text, unsigned digits)
{
	unsigned result = 0;

	while (digits--)
	{
		result <<= 4;
		
		if ( (*text >= '0') && (*text <= '9') )
			result += *text - '0';
		else if ( (*text >= 'A') && (*text <= 'F') )
			result += 10 + *text - 'A';
		else if ( (*text >= 'a') && (*text <= 'f') )
			result += 10 + *text - 'a';

		text++;
	}

	return result;
}

static unsigned calc_modified_crc14(unsigned data, unsigned crc)
{
	unsigned bit, result;

	/*
	this is a modified CRC-14:
	it shifts in 16 (rather than 14) bits
	*/

	for (bit = 0; bit < 16; bit++)
	{
		result = (data & 0x0001) ^ (crc & 0x0001);
		crc >>= 1;
		if (result)
			crc ^= 0x23B1;
		data >>= 1;
	}

	return crc;
}

static const unsigned crc32_table[256] =
{
 0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
 0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
 0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
 0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
 0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
 0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
 0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
 0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
 0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
 0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
 0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
 0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
 0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
 0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
 0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
 0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
 0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
 0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
 0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
 0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
 0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

static unsigned crc32_calc(unsigned crc, unsigned char *buffer, unsigned length)
{
  while (length--)
    crc = crc32_table[(crc ^ *buffer++) & 0xff] ^ (crc >> 8);

  return crc;
}
