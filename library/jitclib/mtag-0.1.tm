package require jitclib::obstackpool
namespace eval ::jitclib {
	variable mtag [list use $obstackpool {*}{
		options		{-Wall -Werror -g}
		export {
			symbols {
				add_mtag_abs
				add_mtag_rel
			}
			header { //@begin=c@
				#ifndef YYCTYPE
				#define _TMP_YYCTYPE 1
				#define YYCTYPE	char
				#endif

				struct mtag_abs {
					struct mtag_abs*	head;
					struct mtag_abs*	next;
					const YYCTYPE*		p;
				};
				//@end=c@@begin=c@

				void add_mtag_abs(struct mtag_abs** pmt, struct obstack* ob, const YYCTYPE* p);
				void shift_all_mtag_abs(struct mtag_abs* mt, ssize_t shift) { //<<<
					for (struct mtag_abs* m = mt->head; m; m=m->next) m->p += shift;
				}
				//@end=c@@begin=c@>>>
				void shift_latest_mtag_abs(struct mtag_abs* mt, ssize_t shift) { //<<<
					mt->p += shift;
				}
				//@end=c@@begin=c@>>>

				struct mtag_rel {
					struct mtag_rel*	head;
					struct mtag_rel*	next;
					ssize_t				dist;
				};
				//@end=c@@begin=c@

				void add_mtag_rel(struct mtag_rel** pmt, struct obstack* ob, ssize_t dist);
				inline void shift_latest_mtag_rel(struct mtag_rel* mt, ssize_t shift) { mt->dist += shift; }

				#ifdef _TMP_YYCTYPE
				#undef YYCTYPE
				#undef _TMP_YYCTYPE
				#endif
			//@end=c@}
		}
		code { //@begin=c@
			#define YYCTYPE	char

			void add_mtag_abs(struct mtag_abs** pmt, struct obstack* ob, const YYCTYPE* p) //<<<
			{
				struct mtag_abs*	mt = obstack_alloc(ob, sizeof *mt);
				if (*pmt) {
					*mt = (struct mtag_abs){
						.head	= (*pmt)->head,
						.p		= p
					};
					(*pmt)->next = mt;
					*pmt = mt;
				} else {
					*mt = (struct mtag_abs){
						.head	= mt,
						.p		= p
					};
					*pmt = mt;
				}
			}

			//@end=c@@begin=c@>>>
			void add_mtag_rel(struct mtag_rel** pmt, struct obstack* ob, ssize_t dist) //<<<
			{
				struct mtag_rel*	mt = obstack_alloc(ob, sizeof *mt);
				if (*pmt) {
					*mt = (struct mtag_rel){
						.head	= (*pmt)->head,
						.dist	= dist
					};
					(*pmt)->next = mt;
					*pmt = mt;
				} else {
					*mt = (struct mtag_rel){
						.head	= mt,
						.dist	= dist
					};
					*pmt = mt;
				}
			}

			//>>>
		//@end=c@}
	}]
}
# vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
