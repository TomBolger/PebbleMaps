"""Compile and exercise the actual queue/callback functions with a fake outbox.

No Pebble SDK needed: the hardware pump is replaced, but queue mutations come
directly from comm.c. Includes every ring head and success/failure callbacks.
"""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parents[1] / 'src/c/comm.c').read_text()

def function(name):
    signature = source.index(name + '(')
    start = source.rfind('\n', 0, signature) + 1
    opening = source.index('{', signature)
    level = 1
    end = opening + 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define QUEUE_LEN 4
typedef struct { int cmd; } OutMsg;
typedef void DictionaryIterator;
typedef int AppMessageResult;
static OutMsg buffer[QUEUE_LEN], *s_queue = buffer;
static int s_q_head, s_q_count, s_retries;
static bool s_sending;
static void *s_retry_timer;
static void retry_cb(void *ctx) {}
static void *app_timer_register(int ms, void (*cb)(void *), void *ctx) { return (void *)1; }
static void pump(void) { if (s_q_count) s_sending = true; }
'''
harness += '\n'.join(function(n) for n in ['comm_send', 'outbox_sent', 'outbox_failed'])
harness += r'''
int main(void) {
  for (int head = 0; head < QUEUE_LEN; head++) {
    for (int failure = 0; failure < 2; failure++) {
      s_q_head = head; s_q_count = 0; s_sending = false; s_retries = 0; s_retry_timer = NULL;
      for (int i = 1; i <= 5; i++) { OutMsg m = { .cmd = i }; comm_send(&m); }
      assert(s_q_count == 4);
      assert(s_queue[s_q_head].cmd == 1); // in flight, not dropped
      assert(s_queue[(s_q_head + 1) % QUEUE_LEN].cmd == 3); // oldest unsent (2) was dropped
      if (failure) {
        for (int i = 0; i < 7; i++) outbox_failed(NULL, 0, NULL);   // gives up after 7 tries
      } else outbox_sent(NULL, NULL);
      assert(s_queue[s_q_head].cmd == 3);
      assert(s_q_count == 3);
      for (int expected = 3; expected <= 5; expected++) {
        assert(s_queue[s_q_head].cmd == expected);
        outbox_sent(NULL, NULL);
      }
      assert(s_q_count == 0);
    }
  }
  return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / 'queue.c').write_text(harness)
    subprocess.run(['gcc', '-std=c99', '-Wall', '-Wno-unused-parameter', str(directory / 'queue.c'), '-o', str(directory / 'queue')], check=True)
    subprocess.run([str(directory / 'queue')], check=True)
print('Queue regression: all ring positions, success and retry exhaustion passed')
