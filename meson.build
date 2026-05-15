project('dpdk-forwarder', 'c',
  version : '0.1.0',
  default_options : ['c_std=gnu11', 'warning_level=2'])

dpdk = dependency('libdpdk', required : true)

sources = files(
  'src/main.c',
  'src/port.c',
  'src/worker.c',
  'src/flow.c',
  'src/stats.c',
  'src/config.c',
)

executable('dpdk-forwarder', sources,
  dependencies : dpdk,
  install : true)

inc = include_directories('src')

# Tests run inside the container with full DPDK (--no-huge --in-memory).
# No shim, no -DUNIT_TEST: the tests exercise the actual rte_hash code paths.
test_flow = executable('test_flow',
  files('tests/test_flow.c', 'src/flow.c'),
  dependencies : dpdk,
  include_directories : inc,
  install : false)

test_stats = executable('test_stats',
  files('tests/test_stats.c', 'src/stats.c', 'src/flow.c'),
  dependencies : dpdk,
  include_directories : inc,
  install : false)

test('flow table',   test_flow,  timeout : 120)
test('stats export', test_stats, timeout : 30)
