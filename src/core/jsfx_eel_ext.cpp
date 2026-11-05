// See jsfx_eel_ext.h. This file includes WDL's eel_strings.h, eel_mdct.h and
// eel_pproc.h, which are implementation headers meant to be compiled once.
#include "jsfx_eel_ext.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "jsfx/ram_pool.h"
#include "jsfx_host.h"

#define WDL_NO_SUPPORT_UTF8

class eel_string_context_state;
static eel_string_context_state* jsfx_ext_strings(void* opaque);
static void jsfx_ext_stdout(void* opaque, const char* text, int length);
extern "C" void jsfx_eel_debugout(const char* format, ...);

#define EEL_STRING_GET_CONTEXT_POINTER(opaque) (jsfx_ext_strings(opaque))
#define EEL_STRING_STDOUT_WRITE(text, length) jsfx_ext_stdout(opaque, text, length)
#include "ptrlist.h"
#include "assocarray.h"
#include "eel_strings.h"
#include "eel_pproc.h"
#include "eel_mdct.h"

// REAPER's atomics guard variables shared with @gfx, which runs on its own
// thread there. Here @gfx is stripped and one thread runs a script's code at a
// time, so they need no lock.
#define EEL_ATOMIC_SET_SCOPE(opaque)
#define EEL_ATOMIC_ENTER
#define EEL_ATOMIC_LEAVE
#include "eel_atomic.h"

namespace jsfx::ext {
namespace {

constexpr int kMaxHandles = 32;
constexpr std::size_t kLogBytes = 8192;
constexpr std::size_t kNameBytes = 512;

struct Handle {
  const JsfxFileData* data = nullptr;
  std::size_t position = 0;       // next value for file_var / file_mem
  std::size_t text_position = 0;  // next byte for file_string
};

}  // namespace

struct State {
  eel_string_context_state strings;
  void* vm = nullptr;

  std::shared_ptr<JsfxFileCache> cache;
  std::shared_ptr<JsfxFileProvider> provider;
  std::vector<FileSlider> file_sliders;
  Handle handles[kMaxHandles];

  // printf output: single producer (the thread running the script), single
  // consumer (the main thread). Bytes that do not fit are dropped.
  char log[kLogBytes];
  std::atomic<std::uint32_t> log_head{0};
  std::atomic<std::uint32_t> log_tail{0};

  char missed[kNameBytes];
  std::atomic<bool> has_missed{false};
};

namespace {

State* state_from(void* opaque) {
  auto* host = static_cast<JsfxHostState*>(opaque);
  return host != nullptr ? static_cast<State*>(host->ext) : nullptr;
}

Handle* handle_from(void* opaque, EEL_F value) {
  State* state = state_from(opaque);
  const int index = static_cast<int>(value + 0.5);
  if (state == nullptr || index < 1 || index > kMaxHandles) return nullptr;
  Handle* handle = &state->handles[index - 1];
  return handle->data != nullptr ? handle : nullptr;
}

// The name file_open() means: a string, or the file chosen in a file slider
// when the argument is that slider's variable. Writes into `out`; no
// allocation, so it is safe on the audio thread.
bool file_name_for(void* opaque, EEL_F* which, char* out, std::size_t out_size) {
  State* state = state_from(opaque);
  auto* host = static_cast<JsfxHostState*>(opaque);
  for (const FileSlider& slider : state->file_sliders) {
    if (host->sliders[slider.index - 1] != which) continue;
    const int choice = static_cast<int>(*which + 0.5);
    if (*which < -0.5 || choice >= static_cast<int>(slider.names.size())) return false;
    const std::string& name = slider.names[static_cast<std::size_t>(choice)];
    if (slider.folder.size() + 1 + name.size() + 1 > out_size) return false;
    snprintf(out, out_size, "%s/%s", slider.folder.c_str(), name.c_str());
    return true;
  }
  const char* name = EEL_STRING_GET_FOR_INDEX(*which, nullptr);
  if (name == nullptr || *name == '\0') return false;
  return normalize_file_name(name, out, out_size);
}

EEL_F NSEEL_CGEN_CALL file_open(void* opaque, EEL_F* which) {
  State* state = state_from(opaque);
  if (state == nullptr) return -1.0;
  char name[kNameBytes];
  if (!file_name_for(opaque, which, name, sizeof(name))) return -1.0;

  const JsfxFileData* data = state->cache != nullptr ? state->cache->find(name) : nullptr;
  if (data == nullptr) {
    if (ram_pool::in_realtime()) {
      // No I/O on the audio thread: report it, and let the host preload.
      if (!state->has_missed.load(std::memory_order_acquire)) {
        snprintf(state->missed, sizeof(state->missed), "%s", name);
        state->has_missed.store(true, std::memory_order_release);
      }
      return -1.0;
    }
    if (state->cache != nullptr) {
      data = state->cache->find_or_open(name, state->provider.get());
    }
  }
  if (data == nullptr) return -1.0;

  for (int i = 0; i < kMaxHandles; ++i) {
    if (state->handles[i].data == nullptr) {
      state->handles[i] = Handle{data, 0, 0};
      return static_cast<EEL_F>(i + 1);
    }
  }
  return -1.0;
}

EEL_F NSEEL_CGEN_CALL file_close(void* opaque, EEL_F* handle_value) {
  Handle* handle = handle_from(opaque, *handle_value);
  if (handle == nullptr) return -1.0;
  *handle = Handle{};
  return 0.0;
}

EEL_F NSEEL_CGEN_CALL file_rewind(void* opaque, EEL_F* handle_value) {
  Handle* handle = handle_from(opaque, *handle_value);
  if (handle == nullptr) return -1.0;
  handle->position = 0;
  handle->text_position = 0;
  return 0.0;
}

EEL_F NSEEL_CGEN_CALL file_avail(void* opaque, EEL_F* handle_value) {
  Handle* handle = handle_from(opaque, *handle_value);
  if (handle == nullptr) return 0.0;
  const std::size_t total = handle->data->values.size();
  return static_cast<EEL_F>(handle->position < total ? total - handle->position : 0);
}

EEL_F NSEEL_CGEN_CALL file_var(void* opaque, EEL_F* handle_value, EEL_F* out) {
  Handle* handle = handle_from(opaque, *handle_value);
  if (handle == nullptr || handle->position >= handle->data->values.size()) return 0.0;
  *out = handle->data->values[handle->position++];
  return 1.0;
}

EEL_F NSEEL_CGEN_CALL file_mem(void* opaque, EEL_F* handle_value, EEL_F* offset,
                               EEL_F* length) {
  Handle* handle = handle_from(opaque, *handle_value);
  State* state = state_from(opaque);
  if (handle == nullptr || state == nullptr || *length < 1.0) return 0.0;
  const std::vector<double>& values = handle->data->values;
  std::size_t wanted = static_cast<std::size_t>(*length);
  wanted = (std::min)(wanted, values.size() - (std::min)(values.size(), handle->position));
  int address = static_cast<int>(*offset + 0.0001);
  std::size_t copied = 0;
  while (copied < wanted && address >= 0) {
    int valid = 0;
    EEL_F* destination =
        NSEEL_VM_getramptr(static_cast<NSEEL_VMCTX>(state->vm), address, &valid);
    if (destination == nullptr || valid <= 0) break;
    const std::size_t chunk = (std::min)(wanted - copied, static_cast<std::size_t>(valid));
    std::copy_n(values.data() + handle->position + copied, chunk, destination);
    copied += chunk;
    address += static_cast<int>(chunk);
  }
  handle->position += copied;
  return static_cast<EEL_F>(copied);
}

EEL_F NSEEL_CGEN_CALL file_riff(void* opaque, EEL_F* handle_value, EEL_F* channels,
                                EEL_F* rate) {
  Handle* handle = handle_from(opaque, *handle_value);
  if (handle == nullptr || handle->data->kind != JsfxFileData::Kind::Audio) {
    *channels = 0.0;
    *rate = 0.0;
    return 0.0;
  }
  *channels = handle->data->channels;
  *rate = handle->data->sample_rate;
  return 1.0;
}

EEL_F NSEEL_CGEN_CALL file_text(void* opaque, EEL_F* handle_value) {
  Handle* handle = handle_from(opaque, *handle_value);
  return handle != nullptr && handle->data->kind == JsfxFileData::Kind::Text ? 1.0 : 0.0;
}

EEL_F NSEEL_CGEN_CALL file_string(void* opaque, EEL_F* handle_value, EEL_F* string_index) {
  Handle* handle = handle_from(opaque, *handle_value);
  if (handle == nullptr) return 0.0;
  EEL_STRING_STORAGECLASS* destination = nullptr;
  EEL_STRING_GET_FOR_WRITE(*string_index, &destination);
  if (destination == nullptr) return 0.0;
  const std::string& text = handle->data->text;
  std::size_t start = (std::min)(handle->text_position, text.size());
  std::size_t end = start;
  while (end < text.size() && text[end] != '\n') ++end;
  const std::size_t next = end < text.size() ? end + 1 : end;
  if (end > start && text[end - 1] == '\r') --end;
  destination->Set(text.data() + start, static_cast<int>(end - start));
  handle->text_position = next;
  return static_cast<EEL_F>(next - start);
}

// --- MIDI as byte buffers and strings ---------------------------------------
// Short messages only: the host carries no SysEx, and SysEx sent this way is
// dropped.

int midi_length(unsigned char status) {
  const int kind = status & 0xF0;
  return kind == 0xC0 || kind == 0xD0 ? 2 : 3;
}

const JsfxMidiEvent* next_midi_in(JsfxHostState* host) {
  if (host == nullptr || host->midi_in_pos >= host->midi_in_count) return nullptr;
  return &host->midi_in[host->midi_in_pos];
}

bool push_midi_out(JsfxHostState* host, EEL_F offset, const unsigned char* bytes,
                   int length) {
  if (length < 1 || bytes[0] < 0x80 || bytes[0] >= 0xF0) return false;
  if (host->midi_out_count >= JSFX_HOST_MAX_MIDI) return false;
  JsfxMidiEvent& event = host->midi_out[host->midi_out_count++];
  event.offset = offset > 0 ? static_cast<int>(offset) : 0;
  event.status = bytes[0];
  event.data1 = length > 1 ? bytes[1] & 0x7F : 0;
  event.data2 = length > 2 ? bytes[2] & 0x7F : 0;
  return true;
}

unsigned char clamp_byte(EEL_F value) {
  const int v = static_cast<int>(value);
  return static_cast<unsigned char>(v < 0 ? 0 : v > 255 ? 255 : v);
}

// midirecv_buf(offset, buf, maxlen): copies the next message into memory and
// returns its length; 0 when none is left.
EEL_F NSEEL_CGEN_CALL midirecv_buf(void* opaque, INT_PTR count, EEL_F** params) {
  auto* host = static_cast<JsfxHostState*>(opaque);
  State* state = state_from(opaque);
  const JsfxMidiEvent* event = next_midi_in(host);
  if (state == nullptr || event == nullptr || count < 3) return 0.0;
  const int length = midi_length(event->status);
  if (*params[2] < length) return 0.0;
  int valid = 0;
  EEL_F* out = NSEEL_VM_getramptr(static_cast<NSEEL_VMCTX>(state->vm),
                                  static_cast<int>(*params[1] + 0.0001), &valid);
  if (out == nullptr || valid < length) return 0.0;
  const unsigned char bytes[3] = {event->status, event->data1, event->data2};
  for (int i = 0; i < length; ++i) out[i] = bytes[i];
  *params[0] = event->offset;
  ++host->midi_in_pos;
  return length;
}

// midisend_buf(offset, buf, len)
EEL_F NSEEL_CGEN_CALL midisend_buf(void* opaque, INT_PTR count, EEL_F** params) {
  auto* host = static_cast<JsfxHostState*>(opaque);
  State* state = state_from(opaque);
  if (state == nullptr || count < 3) return 0.0;
  const int length = static_cast<int>(*params[2]);
  if (length < 1 || length > 3) return 0.0;
  int valid = 0;
  EEL_F* in = NSEEL_VM_getramptr(static_cast<NSEEL_VMCTX>(state->vm),
                                 static_cast<int>(*params[1] + 0.0001), &valid);
  if (in == nullptr || valid < length) return 0.0;
  unsigned char bytes[3] = {0, 0, 0};
  for (int i = 0; i < length; ++i) bytes[i] = clamp_byte(in[i]);
  return push_midi_out(host, *params[0], bytes, length) ? length : 0.0;
}

// midirecv_str(offset, string): the next message as a byte string.
EEL_F NSEEL_CGEN_CALL midirecv_str(void* opaque, INT_PTR count, EEL_F** params) {
  auto* host = static_cast<JsfxHostState*>(opaque);
  const JsfxMidiEvent* event = next_midi_in(host);
  if (event == nullptr || count < 2) return 0.0;
  EEL_STRING_STORAGECLASS* out = nullptr;
  EEL_STRING_GET_FOR_WRITE(*params[1], &out);
  if (out == nullptr) return 0.0;
  const int length = midi_length(event->status);
  const char bytes[3] = {static_cast<char>(event->status),
                         static_cast<char>(event->data1),
                         static_cast<char>(event->data2)};
  out->SetRaw(bytes, length);
  *params[0] = event->offset;
  ++host->midi_in_pos;
  return length;
}

// midisend_str(offset, string)
EEL_F NSEEL_CGEN_CALL midisend_str(void* opaque, INT_PTR count, EEL_F** params) {
  auto* host = static_cast<JsfxHostState*>(opaque);
  if (count < 2) return 0.0;
  EEL_STRING_STORAGECLASS* storage = nullptr;
  const char* text = EEL_STRING_GET_FOR_INDEX(*params[1], &storage);
  if (text == nullptr) return 0.0;
  const int length = storage != nullptr ? storage->GetLength()
                                        : static_cast<int>(std::strlen(text));
  if (length < 1 || length > 3) return 0.0;
  unsigned char bytes[3] = {0, 0, 0};
  for (int i = 0; i < length; ++i) bytes[i] = static_cast<unsigned char>(text[i]);
  return push_midi_out(host, *params[0], bytes, length) ? length : 0.0;
}

// Like WDL's own mdct contexts, created on first use; do that now, on the
// main thread, for every supported length so @sample never allocates them.
void warm_mdct_tables() {
  NSEEL_VMCTX vm = NSEEL_VM_alloc();
  if (vm == nullptr) return;
  NSEEL_VM_setramsize(vm, NSEEL_RAM_ITEMSPERBLOCK);
  std::string code;
  for (int bits = EEL_DCT_MINBITLEN; bits <= EEL_DCT_MAXBITLEN; ++bits) {
    code += "mdct(0," + std::to_string(1 << bits) + ");";
  }
  if (NSEEL_CODEHANDLE handle = NSEEL_code_compile(vm, code.c_str(), 0)) {
    NSEEL_code_execute(handle);
    NSEEL_code_free(handle);
  }
  NSEEL_VM_free(vm);
}

bool parse_config_default(std::string_view line, std::string& name, double& value) {
  // config: name "Description" default [choices...]
  std::size_t p = 0;
  while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
  if (line.substr(p, 7) != "config:") return false;
  p += 7;
  while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
  const std::size_t name_start = p;
  while (p < line.size() && (std::isalnum(static_cast<unsigned char>(line[p])) ||
                             line[p] == '_' || line[p] == '.')) {
    ++p;
  }
  if (p == name_start) return false;
  name.assign(line.substr(name_start, p - name_start));
  while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
  if (p < line.size() && line[p] == '"') {
    p = line.find('"', p + 1);
    if (p == std::string_view::npos) return false;
    ++p;
  }
  while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
  const std::string rest(line.substr(p));
  char* end = nullptr;
  value = std::strtod(rest.c_str(), &end);
  return end != rest.c_str();
}

}  // namespace

void register_functions() {
  EEL_string_register();
  EEL_mdct_register();
  EEL_atomic_register();
  NSEEL_addfunc_varparm("midirecv_buf", 3, NSEEL_PProc_THIS, &midirecv_buf);
  NSEEL_addfunc_varparm("midisend_buf", 3, NSEEL_PProc_THIS, &midisend_buf);
  NSEEL_addfunc_varparm("midirecv_str", 2, NSEEL_PProc_THIS, &midirecv_str);
  NSEEL_addfunc_varparm("midisend_str", 2, NSEEL_PProc_THIS, &midisend_str);
  NSEEL_addfunc_retval("file_open", 1, NSEEL_PProc_THIS, &file_open);
  NSEEL_addfunc_retval("file_close", 1, NSEEL_PProc_THIS, &file_close);
  NSEEL_addfunc_retval("file_rewind", 1, NSEEL_PProc_THIS, &file_rewind);
  NSEEL_addfunc_retval("file_avail", 1, NSEEL_PProc_THIS, &file_avail);
  NSEEL_addfunc_retval("file_var", 2, NSEEL_PProc_THIS, &file_var);
  NSEEL_addfunc_retval("file_mem", 3, NSEEL_PProc_THIS, &file_mem);
  NSEEL_addfunc_retval("file_riff", 3, NSEEL_PProc_THIS, &file_riff);
  NSEEL_addfunc_retval("file_text", 1, NSEEL_PProc_THIS, &file_text);
  NSEEL_addfunc_retval("file_string", 2, NSEEL_PProc_THIS, &file_string);
  warm_mdct_tables();
}

State* create_state(void* vm, JsfxHostState* host, std::shared_ptr<JsfxFileCache> cache,
                    std::shared_ptr<JsfxFileProvider> provider,
                    std::vector<FileSlider> file_sliders) {
  auto* state = new State();
  state->vm = vm;
  state->cache = cache != nullptr ? std::move(cache) : std::make_shared<JsfxFileCache>();
  state->provider = std::move(provider);
  state->file_sliders = std::move(file_sliders);
  host->ext = state;
  NSEEL_VM_SetStringFunc(static_cast<NSEEL_VMCTX>(vm),
                         eel_string_context_state::addStringCallback,
                         eel_string_context_state::addNamedStringCallback);
  return state;
}

void destroy_state(State* state) { delete state; }

void after_compile(State* state, void* vm) {
  if (state != nullptr) state->strings.update_named_vars(static_cast<NSEEL_VMCTX>(vm));
}

bool drain_log(State* state, std::string& out) {
  if (state == nullptr) return false;
  const std::uint32_t head = state->log_head.load(std::memory_order_acquire);
  std::uint32_t tail = state->log_tail.load(std::memory_order_relaxed);
  if (head == tail) return false;
  while (tail != head) {
    out.push_back(state->log[tail % kLogBytes]);
    ++tail;
  }
  state->log_tail.store(tail, std::memory_order_release);
  return true;
}

bool take_missed_file(State* state, std::string& out) {
  if (state == nullptr || !state->has_missed.load(std::memory_order_acquire)) return false;
  out = state->missed;
  state->has_missed.store(false, std::memory_order_release);
  return true;
}

bool preprocess(std::string_view source, std::string& out, std::vector<int>* line_map,
                std::string& error) {
  EEL2_PreProcessor preprocessor;
  std::size_t start = 0;
  while (start < source.size()) {
    std::size_t end = source.find('\n', start);
    if (end == std::string_view::npos) end = source.size();
    std::string name;
    double value = 0.0;
    if (parse_config_default(source.substr(start, end - start), name, value)) {
      preprocessor.define(name.c_str(), value);
    }
    start = end + 1;
  }

  const std::string terminated(source);
  WDL_FastString output;
  if (const char* failure = preprocessor.preprocess(terminated.c_str(), &output)) {
    error = failure;
    return false;
  }
  out.assign(output.Get(), static_cast<std::size_t>(output.GetLength()));

  if (line_map != nullptr) {
    line_map->clear();
    const int lines = static_cast<int>(std::count(out.begin(), out.end(), '\n')) + 1;
    char probe[32];
    for (int line = 1; line <= lines; ++line) {
      snprintf(probe, sizeof(probe), "%d:", line);
      line_map->push_back(std::atoi(preprocessor.translate_error_line(probe)));
    }
  }
  return true;
}

}  // namespace jsfx::ext

static eel_string_context_state* jsfx_ext_strings(void* opaque) {
  jsfx::ext::State* state = jsfx::ext::state_from(opaque);
  return state != nullptr ? &state->strings : nullptr;
}

static void jsfx_ext_stdout(void* opaque, const char* text, int length) {
  jsfx::ext::State* state = jsfx::ext::state_from(opaque);
  if (state == nullptr || text == nullptr || length <= 0) return;
  const std::uint32_t tail = state->log_tail.load(std::memory_order_acquire);
  std::uint32_t head = state->log_head.load(std::memory_order_relaxed);
  for (int i = 0; i < length; ++i) {
    if (head - tail >= jsfx::ext::kLogBytes) break;  // full: drop the rest
    state->log[head % jsfx::ext::kLogBytes] = text[i];
    ++head;
  }
  state->log_head.store(head, std::memory_order_release);
}
