namespace eval ::jitc {
	namespace export *

	apply [list {} {
		variable includepath	{}
		variable librarypath	{}
		variable prefix
		variable packagedir
		variable re2cpath
		variable packccpath
		variable lemonpath

		set dir	[file normalize [file dirname [info script]]]

		set builddir_sentinel	[file join $dir __builddir]
		if {[file readable $builddir_sentinel]} {
			set fd			[open $builddir_sentinel r]
			set packagedir	[try {string trim [read $fd]} finally {close $fd}]
			set re2cpath	[file join $packagedir subprojects/re2c-4.3/re2c]
		} else {
			set packagedir	$dir
			set re2cpath	[file join $packagedir re2c]
		}

		set srcdir_sentinel	[file join $dir __srcdir]
		if {[file readable $srcdir_sentinel]} {
			set fd			[open $srcdir_sentinel r]
			set srcdir		[try {string trim [read $fd]} finally {close $fd}]
			# These aren't staged in the builddir:
			lappend includepath	[file join $srcdir tools/chaos-pp]
			lappend includepath	[file join $srcdir tools/order-pp/inc]
		}

		lappend includepath	[file join $packagedir include]
		set packccpath	[file join $packagedir packcc]
		set lemonpath	[file join $packagedir lemon]
		set jitclib		[file join $packagedir jitclib]

		foreach path [list \
			[tcl::pkgconfig get includedir,runtime] \
			[tcl::pkgconfig get includedir,install] \
		] {
			if {$path ni $includepath} {
				lappend includepath $path
			}
		}

		foreach path [list \
			[tcl::pkgconfig get libdir,runtime] \
			[tcl::pkgconfig get libdir,install] \
		] {
			if {$path ni $librarypath} {
				lappend librarypath $path
			}
		}

		set prefix	[file join {*}[lrange [file split [info nameofexecutable]] 0 end-2]]

		set path	[file join $prefix include]
		if {$path ni $includepath} {
			lappend includepath $path
		}

		set path	[file join $prefix lib]
		if {$path ni $librarypath} {
			lappend librarypath $path
		}

		tcl::tm::path add [file join $packagedir tm]
	} [namespace current]]

	proc _build_compile_error {code errorstr diags args} { #<<<
		# Build a compile failure's errorCode and message.  diags is the structured
		# list the compiler reported: {lvl file line msg extras} per diagnostic
		# (see collect_diag in jitc.c for extras).  errorstr is the plain-text form,
		# used only for failures that come with no diagnostics.
		switch -exact -- [llength $args] {
			0	{}
			2	{lassign $args previous_errmsg previous_options}
			default	{error "Wrong args"}
		}
		set errors	$diags
		if {[llength $errors] == 0} {
			# No structured diagnostics: pick tcc-style "file:line: error: msg" lines out of the text
			set errors	[lmap {- fn line lvl msg} [regexp -all -inline -line {^(.*?):([0-9]+): (error|warning): +(.*?)$} $errorstr] {
				list $lvl $fn $line $msg {}
			}]
			lappend errors	{*}[lmap {- fn lvl msg} [regexp -all -inline -line {^([^:]*): (error|warning): +(.*?)$} $errorstr] {
				list $lvl $fn {} $msg {}
			}]
		}
		set report	{}
		if {[info exists previous_errmsg]} {lappend report $previous_errmsg}
		foreach error $errors {
			lassign $error lvl fn line msg extras
			if {[dict exists $extras option]} {append msg " \[[dict get $extras option]\]"}
			if {$line eq {}} {
				lappend report [format "%s: %s" [string toupper $lvl] $msg]
				continue
			}
			set where	[format {In "%s", line %d} $fn $line]
			if {[dict exists $extras source]} {
				dict with extras {}
				if {$src_name ne $fn || $src_line != $line} {
					append where [format { (generated "%s", line %d)} $src_name $src_line]
				}
				# The diagnosed line of the resolved source, and a caret under the token
				set bol			[expr {$offset - ($column - 1)}]
				set eol			[string first \n $source $offset]
				if {$eol == -1} {set eol [string length $source]}
				set srcline		[string range $source $bol $eol-1]
				set caret		[regsub -all {[^\t]} [string range $srcline 0 $column-2] { }]^
				lappend report [format "%s: %s: %s:\n%s\n%s" [string toupper $lvl] $where $msg $srcline $caret]
			} else {
				lappend report [format "%s: %s: %s:\n%s" [string toupper $lvl] $where $msg [lindex [split $code \n] $line-1]]
			}
		}
		# Nothing parsed out of a non-empty error text: surface it rather than swallow it
		if {[llength $errors] == 0 && [string trim $errorstr] ne {}} {
			lappend report	[string trimright $errorstr]
		}
		list [list JITC COMPILE $errors $code] [join $report \n]
	}

	#>>>

	proc packageinclude {} { #<<<
		variable packagedir
		set packagedir
	}

	#>>>
	proc re2c args { #<<<
		variable re2cpath

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		# Feed the source on stdin directly rather than through a piped `echo`
		# subprocess: re2c is spawned once per compile, so each saved fork+exec
		# counts. The trailing newline matches echo's behaviour.
		exec $re2cpath - --input-encoding utf8 {*}$options << $source\n
	}

	#>>>
	proc packcc args { #<<<
		variable packccpath
		error "Not implemented yet"

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		in_builddir {
			exec echo $source | $packccpath -l -o  {*}$options
		}
	}

	#>>>
	proc lemon args { #<<<
		variable lemonpath
		error "Not implemented yet"

		if {[llength $args] == 0} {
			error "source argument is required"
		}
		set source	[lindex $args end]
		set options	[lrange $args 0 end-1]
		in_builddir {
			exec echo $source | $lemonpath -q {*}$options
		}
	}

	#>>>
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
