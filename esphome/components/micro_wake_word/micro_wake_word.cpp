#include "micro_wake_word.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "audio_preprocessor_int8_model_data.h"

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <cmath>

namespace esphome {
namespace micro_wake_word {

static const char *const TAG = "micro_wake_word";

static const size_t SAMPLE_RATE_HZ = 16000;  // 16 kHz
static const size_t BUFFER_LENGTH = 500;     // 1 seconds
static const size_t BUFFER_SIZE = SAMPLE_RATE_HZ / 1000 * BUFFER_LENGTH;
static const size_t INPUT_BUFFER_SIZE = 32 * SAMPLE_RATE_HZ / 1000;  // 32ms * 16kHz / 1000ms

float MicroWakeWord::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

static const LogString *micro_wake_word_state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return LOG_STR("IDLE");
    case State::START_MICROPHONE:
      return LOG_STR("START_MICROPHONE");
    case State::STARTING_MICROPHONE:
      return LOG_STR("STARTING_MICROPHONE");
    case State::DETECTING_WAKE_WORD:
      return LOG_STR("DETECTING_WAKE_WORD");
    case State::STOP_MICROPHONE:
      return LOG_STR("STOP_MICROPHONE");
    case State::STOPPING_MICROPHONE:
      return LOG_STR("STOPPING_MICROPHONE");
    default:
      return LOG_STR("UNKNOWN");
  }
}

void MicroWakeWord::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Micro Wake Word...");

  if (!this->initialize_models()) {
    ESP_LOGE(TAG, "Failed to initialize models");
    this->mark_failed();
    return;
  }

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  this->input_buffer_ = allocator.allocate(NEW_SAMPLES_TO_GET);
  if (this->input_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate input buffer");
    this->mark_failed();
    return;
  }

  this->ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate ring buffer");
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "Micro Wake Word initialized");
}

int MicroWakeWord::read_microphone_() {
  size_t bytes_read = this->microphone_->read(this->input_buffer_, NEW_SAMPLES_TO_GET * sizeof(int16_t));
  if (bytes_read == 0) {
    return 0;
  }

  size_t bytes_written = this->ring_buffer_->write((void *) this->input_buffer_, bytes_read);
  if (bytes_written != bytes_read) {
    ESP_LOGW(TAG, "Failed to write some data to ring buffer (written=%d, expected=%d)", bytes_written, bytes_read);
  }
  return bytes_written;
}

void MicroWakeWord::loop() {
  switch (this->state_) {
    case State::IDLE:
      break;
    case State::START_MICROPHONE:
      ESP_LOGD(TAG, "Starting Microphone");
      this->microphone_->start();
      this->set_state_(State::STARTING_MICROPHONE);
      this->high_freq_.start();
      break;
    case State::STARTING_MICROPHONE:
      if (this->microphone_->is_running()) {
        this->set_state_(State::DETECTING_WAKE_WORD);
      }
      break;
    case State::DETECTING_WAKE_WORD:
      this->read_microphone_();
      if (this->detect_wakeword()) {
        ESP_LOGD(TAG, "Wake Word Detected");
        this->detected_ = true;
        this->set_state_(State::STOP_MICROPHONE);
      }
      break;
    case State::STOP_MICROPHONE:
      ESP_LOGD(TAG, "Stopping Microphone");
      this->microphone_->stop();
      this->set_state_(State::STOPPING_MICROPHONE);
      this->high_freq_.stop();
      break;
    case State::STOPPING_MICROPHONE:
      if (this->microphone_->is_stopped()) {
        this->set_state_(State::IDLE);
        if (this->detected_) {
          this->detected_ = false;
          this->wake_word_detected_trigger_->trigger("");
        }
      }
      break;
  }
}

void MicroWakeWord::start() {
  if (this->is_failed()) {
    ESP_LOGW(TAG, "Wake word component is marked as failed. Please check setup logs");
    return;
  }
  if (this->state_ != State::IDLE) {
    ESP_LOGW(TAG, "Wake word is already running");
    return;
  }
  this->set_state_(State::START_MICROPHONE);
}

void MicroWakeWord::stop() {
  if (this->state_ == State::IDLE) {
    ESP_LOGW(TAG, "Wake word is already stopped");
    return;
  }
  if (this->state_ == State::STOPPING_MICROPHONE) {
    ESP_LOGW(TAG, "Wake word is already stopping");
    return;
  }
  this->set_state_(State::STOP_MICROPHONE);
}

void MicroWakeWord::set_state_(State state) {
  ESP_LOGD(TAG, "State changed from %s to %s", LOG_STR_ARG(micro_wake_word_state_to_string(this->state_)),
           LOG_STR_ARG(micro_wake_word_state_to_string(state)));
  this->state_ = state;
}

bool MicroWakeWord::initialize_models() {
  ExternalRAMAllocator<uint8_t> arena_allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<int8_t> features_allocator(ExternalRAMAllocator<int8_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<int16_t> audio_samples_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);

  this->streaming_tensor_arena_ = arena_allocator.allocate(STREAMING_MODEL_ARENA_SIZE);
  if (this->streaming_tensor_arena_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the streaming model's tensor arena.");
    return false;
  }

  this->streaming_var_arena_ = arena_allocator.allocate(STREAMING_MODEL_VARIABLE_ARENA_SIZE);
  if (this->streaming_var_arena_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the streaming model variable's tensor arena.");
    return false;
  }

  this->preprocessor_tensor_arena_ = arena_allocator.allocate(PREPROCESSOR_ARENA_SIZE);
  if (this->preprocessor_tensor_arena_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the audio preprocessor model's tensor arena.");
    return false;
  }

  this->new_features_data_ = features_allocator.allocate(PREPROCESSOR_FEATURE_SIZE);
  if (this->new_features_data_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the audio features buffer.");
    return false;
  }

  this->preprocessor_audio_buffer_ = audio_samples_allocator.allocate(MAX_AUDIO_SAMPLE_SIZE * 32);
  if (this->preprocessor_audio_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the audio preprocessor's buffer.");
    return false;
  }

  this->preprocessor_stride_buffer_ = audio_samples_allocator.allocate(HISTORY_SAMPLES_TO_KEEP);
  if (this->preprocessor_stride_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the audio preprocessor's stride buffer.");
    return false;
  }

  this->preprocessor_model_ = tflite::GetModel(g_audio_preprocessor_int8_tflite);
  if (this->preprocessor_model_->version() != TFLITE_SCHEMA_VERSION) {
    ESP_LOGE(TAG, "Wake word's audio preprocessor model's schema is not supported");
    return false;
  }

  this->streaming_model_ = tflite::GetModel(this->model_start_);
  if (this->streaming_model_->version() != TFLITE_SCHEMA_VERSION) {
    ESP_LOGE(TAG, "Wake word's streaming model's schema is not supported");
    return false;
  }

  static tflite::MicroMutableOpResolver<18> preprocessor_op_resolver;
  static tflite::MicroMutableOpResolver<14> streaming_op_resolver;

  if (!this->register_preprocessor_ops_(preprocessor_op_resolver))
    return false;
  if (!this->register_streaming_ops_(streaming_op_resolver))
    return false;

  tflite::MicroAllocator *ma =
      tflite::MicroAllocator::Create(this->streaming_var_arena_, STREAMING_MODEL_VARIABLE_ARENA_SIZE);
  this->mrv_ = tflite::MicroResourceVariables::Create(ma, 15);

  static tflite::MicroInterpreter static_preprocessor_interpreter(
      this->preprocessor_model_, preprocessor_op_resolver, this->preprocessor_tensor_arena_, PREPROCESSOR_ARENA_SIZE);

  static tflite::MicroInterpreter static_streaming_interpreter(this->streaming_model_, streaming_op_resolver,
                                                               this->streaming_tensor_arena_,
                                                               STREAMING_MODEL_ARENA_SIZE, this->mrv_);

  this->preprocessor_interperter_ = &static_preprocessor_interpreter;
  this->streaming_interpreter_ = &static_streaming_interpreter;

  // Allocate tensors for each models.
  if (this->preprocessor_interperter_->AllocateTensors() != kTfLiteOk) {
    ESP_LOGE(TAG, "Failed to allocate tensors for the audio preprocessor");
    return false;
  }
  if (this->streaming_interpreter_->AllocateTensors() != kTfLiteOk) {
    ESP_LOGE(TAG, "Failed to allocate tensors for the streaming model");
    return false;
  }

  this->recent_streaming_probabilities_.resize(this->streaming_model_sliding_window_mean_length_, 0.0);

  return true;
}

bool MicroWakeWord::update_features_() {
  // Verify we have enough samples for a feature slice
  if (!this->slice_available_()) {
    return false;
  }

  // Retrieve strided audio samples
  int16_t *audio_samples = nullptr;
  if (!this->stride_audio_samples_(&audio_samples)) {
    return false;
  }

  // Compute the features for the newest audio samples
  if (!this->generate_single_feature_(audio_samples, SAMPLE_DURATION_COUNT, this->new_features_data_)) {
    return false;
  }

  return true;
}

float MicroWakeWord::perform_streaming_inference_() {
  TfLiteTensor *input = this->streaming_interpreter_->input(0);

  size_t bytes_to_copy = input->bytes;

  memcpy((void *) (tflite::GetTensorData<int8_t>(input)), (const void *) (this->new_features_data_), bytes_to_copy);

  uint32_t prior_invoke = millis();

  TfLiteStatus invoke_status = this->streaming_interpreter_->Invoke();
  if (invoke_status != kTfLiteOk) {
    ESP_LOGW(TAG, "Streaming Interpreter Invoke failed");
    return false;
  }

  ESP_LOGV(TAG, "Streaming Inference Latency=%u ms", (millis() - prior_invoke));

  TfLiteTensor *output = this->streaming_interpreter_->output(0);

  return static_cast<float>(output->data.uint8[0]) / 255.0;
}

bool MicroWakeWord::detect_wakeword() {
  if (!this->update_features_()) {
    return false;
  }

  uint32_t streaming_length = micros();
  float streaming_prob = this->perform_streaming_inference_();

  // Add the most recent probability to the sliding window
  this->recent_streaming_probabilities_[this->last_n_index_] = streaming_prob;
  ++this->last_n_index_;
  if (this->last_n_index_ == this->streaming_model_sliding_window_mean_length_)
    this->last_n_index_ = 0;

  float sum = 0.0;
  for (auto &prob : this->recent_streaming_probabilities_) {
    sum += prob;
  }

  float sliding_window_average = sum / static_cast<float>(this->streaming_model_sliding_window_mean_length_);

  this->ignore_windows_ = std::min(this->ignore_windows_ + 1, 0);
  if (this->ignore_windows_ < 0) {
    return false;
  }

  if (sliding_window_average > this->streaming_model_probability_cutoff_) {
    this->ignore_windows_ = -SPECTROGRAM_LENGTH;
    for (auto &prob : this->recent_streaming_probabilities_) {
      prob = 0;
    }
    return true;
  }

  return false;
}

void MicroWakeWord::set_streaming_model_sliding_window_mean_length(size_t length) {
  this->streaming_model_sliding_window_mean_length_ = length;
  this->recent_streaming_probabilities_.resize(this->streaming_model_sliding_window_mean_length_, 0.0);
}

bool MicroWakeWord::slice_available_() {
  size_t available = this->ring_buffer_->available();
  // ESP_LOGD(TAG, "slices available: %u", available / (NEW_SAMPLES_TO_GET * sizeof(int16_t)));

  if (available > NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
    return true;
  }
  return false;
}

bool MicroWakeWord::stride_audio_samples_(int16_t **audio_samples) {
  // Copy 320 bytes (160 samples over 10 ms) into preprocessor_audio_buffer_ from history in
  // preprocessor_stride_buffer_
  memcpy((void *) (this->preprocessor_audio_buffer_), (void *) (this->preprocessor_stride_buffer_),
         HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

  if (this->ring_buffer_->available() < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
    ESP_LOGD(TAG, "Audio Buffer not full enough");
    return false;
  }

  // Copy 640 bytes (320 samples over 20 ms) from the ring buffer
  // The first 320 bytes (160 samples over 10 ms) will be from history
  size_t bytes_read = this->ring_buffer_->read((void *) (this->preprocessor_audio_buffer_ + HISTORY_SAMPLES_TO_KEEP),
                                               NEW_SAMPLES_TO_GET * sizeof(int16_t), pdMS_TO_TICKS(200));

  if (bytes_read == 0) {
    ESP_LOGE(TAG, "Could not read data from Ring Buffer");
  } else if (bytes_read < NEW_SAMPLES_TO_GET * sizeof(int16_t)) {
    ESP_LOGD(TAG, "Partial Read of Data by Model");
    ESP_LOGD(TAG, "Could only read %d bytes when required %d bytes ", bytes_read,
             (int) (NEW_SAMPLES_TO_GET * sizeof(int16_t)));
    return false;
  }

  // Copy the last 320 bytes (160 samples over 10 ms) from the audio buffer into history stride buffer for the next
  // iteration
  memcpy((void *) (this->preprocessor_stride_buffer_), (void *) (this->preprocessor_audio_buffer_ + NEW_SAMPLES_TO_GET),
         HISTORY_SAMPLES_TO_KEEP * sizeof(int16_t));

  *audio_samples = this->preprocessor_audio_buffer_;
  return true;
}

bool MicroWakeWord::generate_single_feature_(const int16_t *audio_data, const int audio_data_size,
                                             int8_t feature_output[PREPROCESSOR_FEATURE_SIZE]) {
  TfLiteTensor *input = this->preprocessor_interperter_->input(0);
  TfLiteTensor *output = this->preprocessor_interperter_->output(0);
  std::copy_n(audio_data, audio_data_size, tflite::GetTensorData<int16_t>(input));

  if (this->preprocessor_interperter_->Invoke() != kTfLiteOk) {
    ESP_LOGE(TAG, "Failed to preprocess audio for local wake word.");
    return false;
  }
  std::memcpy(feature_output, tflite::GetTensorData<int8_t>(output), PREPROCESSOR_FEATURE_SIZE * sizeof(int8_t));

  return true;
}

bool MicroWakeWord::register_preprocessor_ops_(tflite::MicroMutableOpResolver<18> &op_resolver) {
  if (op_resolver.AddReshape() != kTfLiteOk)
    return false;
  if (op_resolver.AddCast() != kTfLiteOk)
    return false;
  if (op_resolver.AddStridedSlice() != kTfLiteOk)
    return false;
  if (op_resolver.AddConcatenation() != kTfLiteOk)
    return false;
  if (op_resolver.AddMul() != kTfLiteOk)
    return false;
  if (op_resolver.AddAdd() != kTfLiteOk)
    return false;
  if (op_resolver.AddDiv() != kTfLiteOk)
    return false;
  if (op_resolver.AddMinimum() != kTfLiteOk)
    return false;
  if (op_resolver.AddMaximum() != kTfLiteOk)
    return false;
  if (op_resolver.AddWindow() != kTfLiteOk)
    return false;
  if (op_resolver.AddFftAutoScale() != kTfLiteOk)
    return false;
  if (op_resolver.AddRfft() != kTfLiteOk)
    return false;
  if (op_resolver.AddEnergy() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBank() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBankSquareRoot() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBankSpectralSubtraction() != kTfLiteOk)
    return false;
  if (op_resolver.AddPCAN() != kTfLiteOk)
    return false;
  if (op_resolver.AddFilterBankLog() != kTfLiteOk)
    return false;

  return true;
}

bool MicroWakeWord::register_streaming_ops_(tflite::MicroMutableOpResolver<14> &op_resolver) {
  if (op_resolver.AddCallOnce() != kTfLiteOk)
    return false;
  if (op_resolver.AddVarHandle() != kTfLiteOk)
    return false;
  if (op_resolver.AddReshape() != kTfLiteOk)
    return false;
  if (op_resolver.AddReadVariable() != kTfLiteOk)
    return false;
  if (op_resolver.AddStridedSlice() != kTfLiteOk)
    return false;
  if (op_resolver.AddConcatenation() != kTfLiteOk)
    return false;
  if (op_resolver.AddAssignVariable() != kTfLiteOk)
    return false;
  if (op_resolver.AddConv2D() != kTfLiteOk)
    return false;
  if (op_resolver.AddMul() != kTfLiteOk)
    return false;
  if (op_resolver.AddAdd() != kTfLiteOk)
    return false;
  if (op_resolver.AddMean() != kTfLiteOk)
    return false;
  if (op_resolver.AddFullyConnected() != kTfLiteOk)
    return false;
  if (op_resolver.AddLogistic() != kTfLiteOk)
    return false;
  if (op_resolver.AddQuantize() != kTfLiteOk)
    return false;

  return true;
}

}  // namespace micro_wake_word
}  // namespace esphome
