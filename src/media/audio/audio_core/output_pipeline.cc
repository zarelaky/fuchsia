// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/output_pipeline.h"

#include "src/media/audio/audio_core/effects_stage.h"
#include "src/media/audio/audio_core/ring_buffer.h"
#include "src/media/audio/audio_core/tap_stage.h"
#include "src/media/audio/audio_core/usage_settings.h"

namespace media::audio {
namespace {

std::vector<StreamUsage> UsagesFromRenderUsages(const std::vector<RenderUsage>& render_usages) {
  std::vector<StreamUsage> usages;

  std::transform(render_usages.cbegin(), render_usages.cend(), std::back_inserter(usages),
                 [](auto usage) { return StreamUsage::WithRenderUsage(usage); });
  return usages;
}

const Format FormatForMixGroup(const PipelineConfig::MixGroup& mix_group, uint32_t channels) {
  auto result = Format::Create(fuchsia::media::AudioStreamType{
      .sample_format = fuchsia::media::AudioSampleFormat::FLOAT,
      .channels = channels,
      .frames_per_second = mix_group.output_rate,
  });
  FX_DCHECK(result.is_ok());
  return result.take_value();
}

}  // namespace

OutputPipeline::OutputPipeline(const PipelineConfig& config, uint32_t channels,
                               uint32_t max_block_size_frames,
                               TimelineFunction ref_clock_to_fractional_frame,
                               Mixer::Resampler sampler)
    : Stream(FormatForMixGroup(config.root(), channels)) {
  uint32_t usage_mask = 0;
  stream_ =
      CreateMixStage(config.root(), channels, max_block_size_frames,
                     fbl::MakeRefCounted<VersionedTimelineFunction>(ref_clock_to_fractional_frame),
                     &usage_mask, sampler);
}

std::shared_ptr<Mixer> OutputPipeline::AddInput(std::shared_ptr<Stream> stream,
                                                const StreamUsage& usage,
                                                Mixer::Resampler sampler_hint) {
  TRACE_DURATION("audio", "OutputPipeline::AddInput", "stream", stream.get());
  streams_.emplace_back(stream, usage);
  return LookupStageForUsage(usage).AddInput(std::move(stream), sampler_hint);
}

void OutputPipeline::RemoveInput(const Stream& stream) {
  TRACE_DURATION("audio", "OutputPipeline::RemoveInput", "stream", &stream);
  auto it = std::find_if(streams_.begin(), streams_.end(),
                         [&stream](auto& pair) { return pair.first.get() == &stream; });
  FX_CHECK(it != streams_.end());
  LookupStageForUsage(it->second).RemoveInput(stream);
  streams_.erase(it);
}

void OutputPipeline::SetEffectConfig(const std::string& instance_name, const std::string& config) {
  for (auto& effects_stage : effects_stages_) {
    effects_stage->SetEffectConfig(instance_name, config);
  }
}

std::shared_ptr<Stream> OutputPipeline::CreateMixStage(
    const PipelineConfig::MixGroup& spec, uint32_t channels, uint32_t max_block_size_frames,
    fbl::RefPtr<VersionedTimelineFunction> ref_clock_to_fractional_frame, uint32_t* usage_mask,
    Mixer::Resampler sampler) {
  auto output_format = FormatForMixGroup(spec, channels);

  // If we're not running at the output rate, we need to update the timeline function for this stage
  // to compenstate for this rate change.
  auto timeline_function = ref_clock_to_fractional_frame;
  if (output_format.frames_per_second() != format().frames_per_second()) {
    auto timeline_offset = TimelineFunction(
        TimelineRate(output_format.frames_per_second(), format().frames_per_second()));
    timeline_function =
        fbl::MakeRefCounted<DerivedTimelineFunction>(timeline_function, timeline_offset);
  }
  auto stage = std::make_shared<MixStage>(output_format, max_block_size_frames, timeline_function);
  for (const auto& usage : spec.input_streams) {
    auto mask = 1 << static_cast<uint32_t>(usage);
    FX_DCHECK((*usage_mask & mask) == 0);
    *usage_mask |= mask;
  }

  // If we have effects, we should add that stage in now.
  std::shared_ptr<Stream> root = stage;
  if (!spec.effects.empty()) {
    auto effects_stage = EffectsStage::Create(spec.effects, root);
    FX_DCHECK(effects_stage);
    if (effects_stage) {
      effects_stages_.push_back(effects_stage);
      root = std::move(effects_stage);
    }
  }

  // If this is the loopback stage, allocate the loopback ring buffer. Note we want this to be
  // after any effects that may have been applied.
  if (spec.loopback) {
    FX_DCHECK(!loopback_) << "Only a single loopback point is allowed.";
    const uint32_t ring_size = output_format.frames_per_second();
    auto endpoints =
        RingBuffer::AllocateSoftwareBuffer(output_format, ref_clock_to_fractional_frame, ring_size);
    loopback_ = std::move(endpoints.reader);
    root = std::make_shared<TapStage>(std::move(root), std::move(endpoints.writer));
  }

  mix_stages_.emplace_back(stage, UsagesFromRenderUsages(spec.input_streams));
  for (const auto& input : spec.inputs) {
    auto substage = CreateMixStage(input, channels, max_block_size_frames,
                                   ref_clock_to_fractional_frame, usage_mask, sampler);
    stage->AddInput(substage, sampler);
  }
  return root;
}

MixStage& OutputPipeline::LookupStageForUsage(const StreamUsage& usage) {
  for (auto& [mix_stage, stage_usages] : mix_stages_) {
    for (const auto& stage_usage : stage_usages) {
      if (stage_usage == usage) {
        return *mix_stage;
      }
    }
  }
  FX_CHECK(false);
  __UNREACHABLE;
}

}  // namespace media::audio
