#!/usr/bin/env tclsh
# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4

set here	[file dirname [file normalize [info script]]]
lappend auto_path	[file join $here .. local/lib]

package require parse_args
namespace import ::parse_args::*

proc main {} {
	global here argv argv0 _threadinit

	parse_args $argv {
		-load		{-default {}}
		run			{-required}
		args		{-name runargs}
	}
	uplevel #0 $load
	set _threadinit	[subst {
		$load
		set auto_path	[list $::auto_path]
	}]
	append _threadinit {
		tcl::tm::path remove {*}[tcl::tm::path list]
	}
	append _threadinit [subst {
		tcl::tm::path add [lreverse [tcl::tm::path list]]
	}]

	# Swap us out with the research script
	set researchscript	[file join research $run]
	set argv0	$researchscript
	set argv	$runargs
	rename main {}
	tailcall source $researchscript
}

puts stderr "research/run.tcl, argv: ($argv)"

main

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
