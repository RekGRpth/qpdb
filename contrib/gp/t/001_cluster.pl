# Copyright (c) 2026, Georgy Shelkovy
# Licensed under the PostgreSQL License, see LICENSE

# A coordinator and two segments start with their roles

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my %roles;
my @nodes;

foreach my $role ('coordinator', 'segment', 'segment')
{
	my $node = PostgreSQL::Test::Cluster->new($role . scalar(@nodes));
	$node->init;
	$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'gp'\ngp.role = '$role'\n");
	$node->append_conf('postgresql.conf',
		'gp.segment_id = ' . scalar(@nodes))
	  if $role eq 'segment';
	$node->start;
	$roles{ $node->name } = $role;
	push @nodes, $node;
}

foreach my $node (@nodes)
{
	is($node->safe_psql('postgres', 'SHOW gp.role'),
		$roles{ $node->name }, 'role of ' . $node->name);
}

$_->stop foreach @nodes;

done_testing();
