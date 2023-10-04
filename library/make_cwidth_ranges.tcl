package require jitc 0.5.2
package require chantricks

# EastAsianWidth.txt from https://www.unicode.org/Public/UCD/latest/ucd/EastAsianWidth.txt

puts [jitc::capply {
	options		{-Wall -Werror -g}
	filter		{jitc::re2c -W --no-debug-info --case-ranges}
	code {//@begin=c@
		#include <stdint.h>

		static const char hexlut[0xFF] = {
			['0'] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
			['A'] = 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
			['a'] = 0xA, 0xB, 0xC, 0xD, 0xE, 0xF
		};
		//@end=c@@begin=c@

		int parse_hex(const uint8_t* f, const uint8_t* t)
		{
			int acc = 0;
			const uint8_t* p = f;
			while (p < t) {
				acc <<= 4;
				acc |= hexlut[*p++];
			}
			//@end=c@@begin=c@
			return acc;
		}


		void print_codepoint(int c)
		{
			if (c < 0xffff)	printf("\\u%04X", c);
			else			printf("\\U%08X", c);
		}


		OBJCMD(make_ranges)
		{
			int				code = TCL_OK;
			size_t			line;

			enum {A_cmd, A_DEF, A_objc};
			CHECK_ARGS_LABEL(finally, code, "def");
			const uint8_t*	str = (const uint8_t*)Tcl_GetString(objv[A_DEF]);
			const uint8_t*	cur = str;
			const uint8_t*	mar;
			/*!stags:re2c:parseline format = "const uint8_t* @@;"; */
			const uint8_t	*r1, *r2, *r3, *r4, *w;
			int				last_from = -1;
			int				last_to = -1;
			int				range_from, range_to;

			printf("wide\t= [");
			for (line=1;; line++) {
				const uint8_t*	tok = cur;
				/*!local:re2c:parseline
				re2c:api:style			= free-form;
				re2c:define:YYCTYPE		= uint8_t;
				re2c:define:YYCURSOR	= cur;
				re2c:define:YYMARKER	= mar;
				re2c:yyfill:enable		= 0;
				re2c:encoding:utf8		= 1;
				re2c:tags				= 1;

				end			= [\x00];
				eol			= "\n";
				ws			= " "+;
				hexdigit	= [0-9A-Fa-f];
				dot			= [^] \ end \ eol;
				comment		= "#" dot*;

				codepoint	= hexdigit{4,6};
				wide
					= "F"
					| "W";

				narrow
					= "H"
					| "A"
					| "N"
					| "Na";

				width = (@w wide) | narrow;

				@r1 codepoint @r2 ".." @r3 codepoint @r4 ws ";" ws width ws comment? eol	{
					if (w) {
						range_from	= parse_hex(r1, r2);
						range_to	= parse_hex(r3, r4);
						goto combine;
					}
					continue;
				}
				@r1 codepoint @r2 ws ";" ws width ws comment? eol	{
					if (w) {
						range_from	= parse_hex(r1, r2);
						range_to	= range_from;
						goto combine;
					}
					continue;
				}
				comment eol	{ continue; }
				ws? eol		{ continue; }
				end			{ break; }
				*			{ THROW_PRINTF_LABEL(finally, code, "Failed to parse line %d\n", line); }

				*/

			combine:
				if (last_to == -1) {
					last_from = range_from;
					last_to = range_to;
					continue;
				} else if (range_from == last_to+1) {
					last_to = range_to;
					continue;
				} else {
					print_codepoint(last_from);
					if (last_to > last_from) {
						printf("-");
						print_codepoint(last_to);
					}
				}
				last_from = range_from;
				last_to   = range_to;
				continue;
			}

			if (last_from > 0) {
				print_codepoint(last_from);
				if (last_to > last_from) {
					printf("-");
					print_codepoint(last_to);
				}
			}
			printf("];\n");

		finally:
			return code;
		}
		//@end=c@
	}
} make_ranges [chantricks::readfile /tmp/EastAsianWidth.txt]]

