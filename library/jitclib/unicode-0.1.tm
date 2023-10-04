file mkdir /tmp/jitclib_unicode
file delete {*}[glob -type f -nocomplain /tmp/jitclib_unicode/*]
namespace eval ::jitclib {
	variable unicode {
		debug /tmp/jitclib_unicode
		filter	{jitc::re2c -W --case-ranges --no-debug-info}
		export {
			symbols {
				utf8_to_codepoint
				utf8_charcols
				utf8_step_back
				utf8_charlen_bounded
				utf8_append_printable
				utf8_make_printable
			}
			header { //@begin=c@
				#include <stdint.h>

				/*!rules:re2c:unicode 

				unicode_unprintable	= [\u0000-\u0008\u000B-\u001F\u007F-\u009F\u2000-\u200F\u2028-\u202F\u205F-\u206F\u3000\uFEFF\U000E0100-\U000E01EF];
				unicode_printable	= [^] \ unicode_unprintable;

				// Characters that take up 2 columns in a terminal
				unicode_wide		= [\u1100-\u115F\u231A-\u231B\u2329-\u232A\u23E9-\u23EC\u23F0\u23F3\u25FD-\u25FE\u2614-\u2615\u2648-\u2653\u267F\u2693\u26A1\u26AA-\u26AB\u26BD-\u26BE\u26C4-\u26C5\u26CE\u26D4\u26EA\u26F2-\u26F3\u26F5\u26FA\u26FD\u2705\u270A-\u270B\u2728\u274C\u274E\u2753-\u2755\u2757\u2795-\u2797\u27B0\u27BF\u2B1B-\u2B1C\u2B50\u2B55\u2E80-\u2E99\u2E9B-\u2EF3\u2F00-\u2FD5\u2FF0-\u303E\u3041-\u3096\u3099-\u30FF\u3105-\u312F\u3131-\u318E\u3190-\u31E3\u31EF-\u321E\u3220-\u3247\u3250-\u4DBF\u4E00-\uA48C\uA490-\uA4C6\uA960-\uA97C\uAC00-\uD7A3\uF900-\uFAFF\uFE10-\uFE19\uFE30-\uFE52\uFE54-\uFE66\uFE68-\uFE6B\uFF01-\uFF60\uFFE0-\uFFE6\U00016FE0-\U00016FE4\U00016FF0-\U00016FF1\U00017000-\U000187F7\U00018800-\U00018CD5\U00018D00-\U00018D08\U0001AFF0-\U0001AFF3\U0001AFF5-\U0001AFFB\U0001AFFD-\U0001AFFE\U0001B000-\U0001B122\U0001B132\U0001B150-\U0001B152\U0001B155\U0001B164-\U0001B167\U0001B170-\U0001B2FB\U0001F004\U0001F0CF\U0001F18E\U0001F191-\U0001F19A\U0001F200-\U0001F202\U0001F210-\U0001F23B\U0001F240-\U0001F248\U0001F250-\U0001F251\U0001F260-\U0001F265\U0001F300-\U0001F320\U0001F32D-\U0001F335\U0001F337-\U0001F37C\U0001F37E-\U0001F393\U0001F3A0-\U0001F3CA\U0001F3CF-\U0001F3D3\U0001F3E0-\U0001F3F0\U0001F3F4\U0001F3F8-\U0001F43E\U0001F440\U0001F442-\U0001F4FC\U0001F4FF-\U0001F53D\U0001F54B-\U0001F54E\U0001F550-\U0001F567\U0001F57A\U0001F595-\U0001F596\U0001F5A4\U0001F5FB-\U0001F64F\U0001F680-\U0001F6C5\U0001F6CC\U0001F6D0-\U0001F6D2\U0001F6D5-\U0001F6D7\U0001F6DC-\U0001F6DF\U0001F6EB-\U0001F6EC\U0001F6F4-\U0001F6FC\U0001F7E0-\U0001F7EB\U0001F7F0\U0001F90C-\U0001F93A\U0001F93C-\U0001F945\U0001F947-\U0001F9FF\U0001FA70-\U0001FA7C\U0001FA80-\U0001FA88\U0001FA90-\U0001FABD\U0001FABF-\U0001FAC5\U0001FACE-\U0001FADB\U0001FAE0-\U0001FAE8\U0001FAF0-\U0001FAF8\U00020000-\U0002FFFD\U00030000-\U0003FFFD];
				*/

				uint32_t utf8_to_codepoint(const uint8_t* cur, int* len);
				size_t utf8_charcols(const char* str);
				const uint8_t* utf8_step_back(const uint8_t* from, int chars, const uint8_t* start);
				size_t utf8_charlen_bounded(const char* str, const char* until);
				void utf8_append_printable(Tcl_DString* ds, const uint8_t** cur, const uint8_t* end, int end_ellipsis);
				const char* utf8_make_printable(const uint8_t* cur);
			//@end=c@}
		}
		code { //@begin=c@
			Tcl_DString		g_staticbuf;

			INIT { //<<<
				Tcl_DStringInit(&g_staticbuf);
				return TCL_OK;
			}

			//@end=c@@begin=c@>>>
			RELEASE { //<<<
				Tcl_DStringFree(&g_staticbuf);
			}

			//@end=c@@begin=c@>>>
			uint32_t utf8_to_codepoint(const uint8_t* cur, int* len) //<<<
			{
				const uint8_t*	c = cur;
				uint32_t		codepoint = 0;
				if ((*c & 0x80) == 0) {
					codepoint = *c++;
				} else if ((*c & 0xE0) == 0xC0) { // Two byte encoding
					codepoint  = (*c++ & 0x1F) << 6;
					codepoint |= (*c++ & 0x3F);
				} else if ((*c & 0xF0) == 0xE0) { // Three byte encoding
					codepoint  = (*c++ & 0x0F) << 12;
					codepoint |= (*c++ & 0x3F) << 6;
					codepoint |= (*c++ & 0x3F);
				} else if ((*c & 0xF8) == 0xF0) { // Four byte encoding
					codepoint  = (*c++ & 0x07) << 18;
					codepoint |= (*c++ & 0x3F) << 12;
					codepoint |= (*c++ & 0x3F) << 6;
					codepoint |= (*c++ & 0x3F);
				} else if ((*c & 0xFC) == 0xF8) { // Five byte encoding
					codepoint  = (*c++ & 0x03) << 24;
					codepoint |= (*c++ & 0x3F) << 18;
					codepoint |= (*c++ & 0x3F) << 12;
					codepoint |= (*c++ & 0x3F) << 6;
					codepoint |= (*c++ & 0x3F);
				} else if ((*c & 0xFE) == 0xFC) { // Six byte encoding
					codepoint  = (*c++ & 0x01) << 30;
					codepoint |= (*c++ & 0x3F) << 24;
					codepoint |= (*c++ & 0x3F) << 18;
					codepoint |= (*c++ & 0x3F) << 12;
					codepoint |= (*c++ & 0x3F) << 6;
					codepoint |= (*c++ & 0x3F);
				} else {	// Invalid encoding
					codepoint  = 0xFFFD;		// replacement character
					c++;
				}
				if (len) *len = c - cur;
				return codepoint;
			}

			//@end=c@@begin=c@>>>
			size_t utf8_charcols(const char* str) // Count CESU-8 column widths until null <<<
			{
				const uint8_t*	cur = (const uint8_t*)str;
				const uint8_t*	mar;
				size_t			count = 0;

				for (;;) {
					const uint8_t*	tok = cur;
					/*!local:re2c:unicode_charcols
					!use:unicode;
					re2c:api:style				= free-form;
					re2c:define:YYCTYPE			= uint8_t;
					re2c:define:YYCURSOR		= cur;
					re2c:define:YYMARKER		= mar;
					re2c:yyfill:enable			= 0;
					re2c:encoding:utf8			= 1;

					end		= [\x00];
					char	= [^] \ end;

					end					{ break; }
					unicode_wide		{ count+=2; continue; }
					unicode_unprintable	{           continue; }
					char				{ count++;  continue; }
					* {
						if (tok[0] == 0xC0 && tok[1] == 0x80) {
							// CESU-8 encoding of codepoint 0, count 1
							// character.  re2c doesn't match this as a
							// character because UTF-8 doesn't accept encodings
							// of codepoints that are longer than necessary.
							// \u0000 is not printable though, so don't count it.
							continue;
						}
						// Treat an invalid codeunit as a character (that would likely be replaced by \ufffd (replacement character) when displayed)
						count++;
						continue;
					}
					*/
				}

				return count;
			}

			//@end=c@@begin=c@>>>
			const uint8_t* utf8_step_back(const uint8_t* from, int chars, const uint8_t* start) //<<<
			{
				const uint8_t*	p = from;
				size_t	remaining = chars;
				for (; remaining && p>=start;) {
					for (; p>=start;) {
						p--;
						const uint8_t ch = p[0];
						if (
							(ch & 0b10000000 == 0) ||
							(ch & 0b11000000 == 0b11000000)
						) {
							remaining--;
							break;
						};
					}
				}
				return p;
			}

			//>>>
			size_t utf8_charlen_bounded(const char* str, const char* until) // Count CESU-8 chars in range <<<
			{
				const uint8_t*	cur = (const uint8_t*)str;
				const uint8_t*	mar;
				size_t			count;

				for (count = 0; cur < (const uint8_t*)until; count++) {
					const uint8_t*	tok = cur;
					/*!local:re2c:charlen_bounded
					!use:unicode;
					re2c:api:style				= free-form;
					re2c:define:YYCTYPE			= uint8_t;
					re2c:define:YYCURSOR		= cur;
					re2c:define:YYMARKER		= mar;
					re2c:yyfill:enable			= 0;
					re2c:encoding:utf8			= 1;

					end		= [\x00];
					char	= [^] \ end;

					end		{ break; }
					char	{ continue; }
					* {
						if (tok[0] == 0xC0 && tok[1] == 0x80) {
							// CESU-8 encoding of codepoint 0, count 1
							// character.  re2c doesn't match this as a
							// character because UTF-8 doesn't accept encodings
							// of codepoints that are longer than necessary.
							count++;
							continue;
						}
						// Treat an invalid codeunit as a character (that would likely be replaced by \ufffd (replacement character) when displayed)
						count++;
						continue;
					}
					*/
				}

				return count;
			}

			//>>>
			void utf8_append_printable(Tcl_DString* ds, const uint8_t** cur, const uint8_t* end, int end_ellipsis) //<<<
			{
				static const char	hexdigits[0xF+1] = "0123456789ABCDEF";
				const uint8_t*		mar;

				// *cur might be in the middle of a character, step forward until we reach a valid boundary
				while (*cur < end) {
					const uint8_t*	tok = *cur;
					/*!local:re2c:append_printable1
					re2c:api:style				= free-form;
					re2c:define:YYCTYPE			= uint8_t;
					re2c:define:YYCURSOR		= (*cur);
					re2c:define:YYMARKER		= mar;
					re2c:yyfill:enable			= 0;
					re2c:encoding:utf8			= 1;

					end		= [\x00];

					[^]	\ end	{ *cur = tok; break; }
					end			{ break; }
					*	{
						if (tok[0] == 0xC0 && tok[1] == 0x80); *cur = tok; break;
						continue;
					}
					*/
				}

				while (*cur < end) {
					const uint8_t* tok = *cur;
					/*!local:re2c:append_printable2
					!use:unicode;
					re2c:api:style				= free-form;
					re2c:define:YYCTYPE			= uint8_t;
					re2c:define:YYCURSOR		= (*cur);
					re2c:define:YYMARKER		= mar;
					re2c:yyfill:enable			= 0;
					re2c:encoding:utf8			= 1;

					end		= [\x00];

					end			{ break; }
					"\b"		{ Tcl_DStringAppend(ds, "\\b", 2); continue; }
					"\f"		{ Tcl_DStringAppend(ds, "\\f", 2); continue; }
					"\n"		{ Tcl_DStringAppend(ds, "\\n", 2); continue; }
					"\r"		{ Tcl_DStringAppend(ds, "\\r", 2); continue; }
					"\t"		{ Tcl_DStringAppend(ds, "\\t", 2); continue; }
					unicode_printable	{ Tcl_DStringAppend(ds, (const char*)tok, (*cur)-tok); continue; }
					unicode_unprintable	{
						int consumed;
						const uint32_t ch = utf8_to_codepoint(tok, &consumed);
						if (consumed != (*cur)-tok) Tcl_Panic("utf8_to_codepoint disagrees with re2c about the character encoding length: %d vs %d", consumed, (*cur)-tok);
						if (ch <= 0xff) {
							Tcl_DStringAppend(ds, "\\x", 2);
							Tcl_DStringAppend(ds, hexdigits+(ch >> 4 ), 1);
							Tcl_DStringAppend(ds, hexdigits+(ch & 0xF), 1);
						} else if (ch <= 0xffff) {
							Tcl_DStringAppend(ds, "\\u", 2);
							Tcl_DStringAppend(ds, hexdigits+((ch >> 12) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >>  8) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >>  4) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+( ch        & 0xF ), 1);
						} else {
							Tcl_DStringAppend(ds, "\\U", 2);
							Tcl_DStringAppend(ds, hexdigits+((ch >> 28) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >> 24) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >> 20) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >> 16) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >> 12) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >>  8) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+((ch >>  4) & 0xF ), 1);
							Tcl_DStringAppend(ds, hexdigits+( ch        & 0xF ), 1);
						}
						continue;
					}
					*	{
						if (tok[0] == 0xC0 && tok[1] == 0x80) {
							// cesu8 null
							Tcl_DStringAppend(ds, "\\x00", 4);
							continue;
						}
						Tcl_DStringAppend(ds, "\\x", 2);
						Tcl_DStringAppend(ds, hexdigits+(tok[0] >> 4 ), 1);
						Tcl_DStringAppend(ds, hexdigits+(tok[0] & 0xF), 1);
						continue;
					}
					*/
				}

				if (*cur == end && end_ellipsis)
					Tcl_DStringAppend(ds, "\u2026", -1);
			}

			//>>>
			const char* utf8_make_printable(const uint8_t* c) //<<<
			{
				const uint8_t*	cur = c;
				Tcl_DStringSetLength(&g_staticbuf, 0);
				utf8_append_printable(&g_staticbuf, &cur, cur+1, 0);
				return Tcl_DStringValue(&g_staticbuf);
			}

			//>>>
		//@end=c@}
	}
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
