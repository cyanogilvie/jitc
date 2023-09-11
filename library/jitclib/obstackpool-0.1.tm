package require jitclib::tcltime
namespace eval ::jitclib {
	variable obstackpool	[list use $::jitclib::tcltime mode raw options {-Wall} export {
		symbols {
			obstack_pool_get
			obstack_pool_release
			obstack_pool_groom
		}
		header { //@begin=c@<<<
			#include <stdint.h>
			#include <obstack.h>

			enum obstack_pool_estimate {
				OBSTACK_POOL_SMALL,			// A few pages
				OBSTACK_POOL_MEDIUM			// A MB or more
			};

			//@end=c@@begin=c@
			struct obstack* obstack_pool_get(enum obstack_pool_estimate est);
			void obstack_pool_release(struct obstack* ob);
			void obstack_pool_groom(uint64_t ts);
			//@end=c@>>>
		}
	} code { //@begin=c@ <<<
		#include <stdio.h>
		#include <stdlib.h>

		struct obstack_slot {
			struct obstack			ob;		// Must be first
			struct obstack_slot*	next;
			uint64_t				last_returned;
			void*					first_object;
		};

		//@end=c@@begin=c@
		struct obstack_pool {
			struct obstack_slot*	free;
			int						avail;
			uint64_t				oldest;
		};

		#define obstack_chunk_alloc	malloc
		#define obstack_chunk_free	free

		struct obstack_pool	g_obstack = {0};

		struct obstack* obstack_pool_get(enum obstack_pool_estimate est) //<<<
		{
			struct obstack_slot*	slot;

			if (g_obstack.free) {
				slot = g_obstack.free;
				g_obstack.free = slot->next;
				slot->next = NULL;
				g_obstack.avail--;
				//fprintf(stderr, "returning obstack from the pool: %p, slot->next: %p, g_obstack.free: %p, g_obstack.avail: %d\n", slot, slot->next, g_obstack.free, g_obstack.avail);
				return (struct obstack*)slot;
			}

			slot = (struct obstack_slot*)obstack_chunk_alloc(sizeof(struct obstack_slot));
			*slot = (struct obstack_slot){0};
			switch (est) {
				case OBSTACK_POOL_SMALL:
					obstack_begin(&slot->ob, 12288-32);
					break;
				case OBSTACK_POOL_MEDIUM:
					obstack_begin(&slot->ob, 1048576-32);
					break;
				default:
					obstack_begin(&slot->ob, 12288-32);
			}
			slot->first_object = obstack_alloc(&slot->ob, 1);	// Record the first obstack allocation so we can release it when the obstack is returned

			return (struct obstack*)slot;
		}

		//>>>
		void obstack_pool_release(struct obstack* ob) //<<<
		{
			struct obstack_slot*	slot = (struct obstack_slot*)ob;
			const uint64_t			ts = now();

			obstack_free(&slot->ob, slot->first_object);
			slot->first_object = obstack_alloc(&slot->ob, 1);

			slot->next = g_obstack.free;
			slot->last_returned = ts;

			if (g_obstack.free == NULL)
				g_obstack.oldest = ts;

			g_obstack.free = slot;
			g_obstack.avail++;

			//fprintf(stderr, "returned obstack: %p, slot->next: %p, g_obstack.free: %p, g_obstack.avail: %d\n", slot, slot->next, g_obstack.free, g_obstack.avail);

			obstack_pool_groom(ts);
		}

		//>>>
		void obstack_pool_groom(uint64_t ts) //<<<
		{
			const int		min_pool = 10;		// Keep at least this many slots in the pool
			const uint64_t	horizon = ts - 30*1000000ULL;	// Free excess slots that are older than this

			if (
					g_obstack.avail > min_pool &&
					g_obstack.oldest < horizon
			) {
				int						i = 0;
				struct obstack_slot*	s = g_obstack.free;
				struct obstack_slot*	p = NULL;

				while (s) {
					if (++i > min_pool && s->last_returned > horizon) {
						// Free this slot and all trailing ones (which are guaranteed to be older)
						p->next = NULL;
						g_obstack.oldest = p->last_returned;
						g_obstack.avail = i-1;
						while (s) {
							obstack_free(&s->ob, NULL);
							s = s->next;
						}
						break;
					}
					p = s;
					s = s->next;
				}
			}
		}

		//>>>

		void release(void* interp)
		{
			struct obstack_slot*	s = g_obstack.free;
			struct obstack_slot*	n = NULL;
			int i = 0;
			while (s) {
				n = s->next;
				//fprintf(stderr, "Freeing obstack_slot: %p\n", s);
				obstack_free(&s->ob, NULL);
				obstack_chunk_free(s);
				if (i++ > 3) break;
				s = n;
			}
			g_obstack = (struct obstack_pool){0};
		}
		//@end=c@ >>>
	}]
}
# vim: foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
