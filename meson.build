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

# Unit tests — compiled without DPDK runtime via -DUNIT_TEST
test_flow = executable('test_flow',
  ['tests/test_flow.c', 'src/flow.c'],
  c_args : ['-DUNIT_TEST'],
  include_directories : include_directories('src'),
  install : false)

test_stats = executable('test_stats',
  ['tests/test_stats.c', 'src/stats.c'],
  c_args : ['-DUNIT_TEST'],
  include_directories : include_directories('src'),
  install : false)

test('flow table',  test_flow)
test('stats export', test_stats)
