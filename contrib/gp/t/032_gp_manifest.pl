# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# The manifest of the answers of our own: their base answer is hashed at
# the moment they are written, so a merge of upstream that changes a base
# answer is caught before it turns the tests of the cluster red, and an
# answer that grew equal to its base again is asked for removal

use strict;
use warnings FATAL => 'all';
use File::Copy qw(copy);
use File::Path qw(make_path);
use File::Temp qw(tempdir);
use IPC::Run qw(run);
use Test::More;

# The script under test, from the tree of the extension
my $script = $0;
$script =~ s{[/\\]t[/\\]\d+[^/\\]*\.pl$}{};
$script .= '/tools/gp-gp-manifest.sh';
plan skip_all => "$script not found" unless -f $script;

# The shell of the script: named in full, for the command line of
# Windows does not find sh by the paths of its environment
my $sh = 'sh';
if ($^O eq 'MSWin32')
{
	for my $candidate (qw(C:/msys64/usr/bin/sh.exe
		C:/Program Files/Git/usr/bin/sh.exe))
	{
		if (-f $candidate)
		{
			$sh = $candidate;
			last;
		}
	}
	plan skip_all => 'no sh for the manifest script'
		if $sh eq 'sh' && !run([$sh, '-c', 'exit']);
}

# A git repository of its own: the script walks to the tree root by git.
# Every shell command runs as IPC::Run with the named shell, which needs
# neither the command line of Windows nor its paths; and the shell of msys
# does not keep the directory of the Windows command line, so the change of
# the directory happens inside the shell itself
my $repo = tempdir(CLEANUP => 1);
$repo =~ s{\\}{/}g if $^O eq 'MSWin32';
make_path("$repo/contrib/gp/pgtests/expected",
		  "$repo/contrib/gp/tools", "$repo/src/test/regress/expected");
copy($script, "$repo/contrib/gp/tools/gp-gp-manifest.sh")
	or die "copy $script: $!";

# The base answers of upstream and an answer of our own made from them
write_file("$repo/src/test/regress/expected/foo.out", "the base answer\n");
write_file("$repo/contrib/gp/pgtests/expected/foo.gp.out",
	"the base answer\nthe part of the cluster\n");

run_git('init');

# update writes the manifest of every answer of our own
is(sh_out('update'), '', 'update writes the manifest');
ok(-f "$repo/contrib/gp/pgtests/MANIFEST", 'the manifest is there');
my $manifest = read_file("$repo/contrib/gp/pgtests/MANIFEST");
like($manifest, qr/^foo [0-9a-f]{64}$/m, 'it hashes the base answer');

# check passes while the base is what the answer was made from
is(sh_out('check'), '', 'check passes on a fresh answer');

# A base answer that changed since leaves the answer of our own stale
write_file("$repo/src/test/regress/expected/foo.out",
	"the base answer, changed upstream\n");
like(sh_out('check'), qr/foo: base answer changed/,
	'check names the stale answer');

# An answer that equals its base again is asked for removal
write_file("$repo/src/test/regress/expected/foo.out", "the base answer\n");
write_file("$repo/contrib/gp/pgtests/expected/foo.gp.out",
	"the base answer\n");
like(sh_out('check'), qr/foo: answer equals its base, remove it/,
	'check asks for the removal of a redundant answer');

# An answer with no manifest entry is reported; update fixes it
write_file("$repo/contrib/gp/pgtests/expected/bar.gp.out",
	"another base\nthe part of the cluster\n");
write_file("$repo/src/test/regress/expected/bar.out", "another base\n");
like(sh_out('check'), qr/no manifest entry for bar/,
	'check reports a missing entry');
is(sh_out('update'), '', 'update takes the new answer in');
ok(read_file("$repo/contrib/gp/pgtests/MANIFEST") =~ /^bar [0-9a-f]{64}$/m,
	'the manifest has the new entry');

done_testing();

sub write_file
{
	my ($path, $content) = @_;

	open(my $fh, '>', $path) or die "open $path: $!";
	print $fh $content;
	close $fh;
}

sub read_file
{
	my ($path) = @_;
	my $content;

	open(my $fh, '<', $path) or die "open $path: $!";
	local $/;
	$content = <$fh>;
	close $fh;
	return $content;
}

# Run git in the repository through the named shell
sub run_git
{
	my ($command) = @_;
	my ($out, $err);

	run([$sh, '-c', "cd '$repo' && git $command"], '>', \$out, '2>', \$err)
		or die "git $command: $?\n$out$err";
}

# Run the script in the repository and return its output with the errors;
# the command is expected to succeed when a test asks for an empty answer
sub sh_out
{
	my ($command) = @_;
	my $out;

	run([$sh, '-c',
		"cd '$repo' && sh contrib/gp/tools/gp-gp-manifest.sh $command 2>&1"],
		'>', \$out);
	return $out;
}
