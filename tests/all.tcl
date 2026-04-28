package require tcltest 2.5
namespace import ::tcltest::test

::tcltest::configure -singleproc 1 {*}$argv -testdir [file dirname [info script]]

# We need to ensure that the testsDirectory is absolute
::tcltest::normalizePath ::tcltest::testsDirectory

set failed [::tcltest::runAllTests]

::tcltest::cleanupTests 0

if {$failed} {
    puts $::tcltest::outputChannel "[file tail [info script]]: $failed test(s) failed"
    close stderr
    error "test run failed"
}
