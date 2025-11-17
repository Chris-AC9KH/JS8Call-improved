#include "Modulator.hpp"
#include <limits>
#include <numbers>
#include <QDateTime>
#include <QLoggingCategory>
#include <QtMath>
#include "commons.h"
#include "DriftingDateTime.h"
#include "JS8Submode.hpp"
#include "mainwindow.h"
#include "soundout.h"

#include "moc_Modulator.cpp"

Q_DECLARE_LOGGING_CATEGORY(modulator_js8)

namespace
{
  constexpr double TAU        = 2 * std::numbers::pi;
  constexpr auto   FRAME_RATE = 48000;
  constexpr auto   MS_PER_DAY = 86400000;
  constexpr auto   MS_PER_SEC = 1000;
}

void
Modulator::start(double        const frequency,
                 int           const submode,
                 double        const txDelay,
                 SoundOutput * const stream,
                 Channel       const channel)
{
  Q_ASSERT (stream);

  const State current_state = m_state.load();
  if (current_state != State::Idle) {
      qCDebug(modulator_js8)
          << "Modulator does not find itself in state idle, but"
          << (current_state == State::Active ? "Active" :
              current_state == State::Synchronizing ? "Synchronizing" :
              current_state == State::Idle ? "Idle" : "??What??")
          << "so calling stop()";
      stop();
  }

  m_quickClose      = false;
  m_audioFrequency  = frequency;
  m_nsps            = JS8::Submode::samplesForOneSymbol(submode);
  m_toneSpacing     = JS8::Submode::toneSpacing(submode);
  m_isym0           = std::numeric_limits<unsigned>::max();
  m_amp             = std::numeric_limits<qint16>::max();
  m_audioFrequency0 = 0.0;
  m_phi             = 0.0;
  m_silentFrames    = 0;
  m_ic              = 0;

  // If we're not tuning, then we'll need to figure out exactly when we
  // should start transmitting; this will depend on the submode in play.

  if (!m_tuning)
  {
    // Get the nominal transmit start time for this submode, and determine
    // which millisecond of the current transmit period we're currently at.

    qint64   const nowMS = DriftingDateTime::currentMSecsSinceEpoch();
    unsigned const periodMS = JS8::Submode::period(submode) * MS_PER_SEC;
    auto     const startDelayMS = JS8::Submode::startDelayMS(submode);
    unsigned const periodOffsetMS =  nowMS % periodMS;

    // If we haven't yet hit the nominal start time for the period, then we
    // will need to inject some silence into the transmission; determine the
    // number of silent audio samples required to start audio at the correct amount
    // of delay into the period.
    //
    // If we have hit the nominal start time for the period, adjust for late
    // start if we're not exactly at the nominal start time.

    bool const inTxDelayBeforePeriodStart = periodMS <= periodOffsetMS + txDelay * MS_PER_SEC;
    if (inTxDelayBeforePeriodStart) {
        unsigned const additionalMSNeededForTxDelay = periodMS - periodOffsetMS;
        qCDebug(modulator_js8) << "Sending" << additionalMSNeededForTxDelay
                               << "ms silence for TX delay.";
        m_silentFrames = (startDelayMS + additionalMSNeededForTxDelay) * FRAME_RATE / MS_PER_SEC;
    } else if (startDelayMS > periodOffsetMS) {
        qCDebug(modulator_js8) << "Starting" << periodOffsetMS
                               << "ms late into transmission, skipping some of the"
                               << startDelayMS << "ms start delay";
        m_silentFrames = (startDelayMS - periodOffsetMS) * FRAME_RATE / MS_PER_SEC;
    } else {
        qCWarning(modulator_js8) << "Starting" << periodOffsetMS
                                 << "ms late into transmission, cutting away initial symbol(s).";
        m_ic = (periodOffsetMS - startDelayMS) * FRAME_RATE / MS_PER_SEC;
    }
  } else {
      qCDebug(modulator_js8) << "Modulator finds it is tuning.";
  }

  initialize(QIODevice::ReadOnly, channel);

  if (0 < m_silentFrames) {
      m_state.store(State::Synchronizing);
      qCDebug(modulator_js8)
          << "Symbol transmission to start after"
          << ((float) m_silentFrames) / FRAME_RATE * MS_PER_SEC
          << "ms of silence.";
  } else {
      m_state.store(State::Active);
      qCDebug(modulator_js8) << "Symbol transmission to start immediately.";
  }

  m_stream = stream;
  if (m_stream)
  {
    m_stream->restart(this);
  }
  else
  {
    qCDebug(modulator_js8) << "Modulator::start: no audio output stream assigned";
  }
}

void
Modulator::tune(bool const tuning)
{
  m_tuning = tuning;
  if (!m_tuning) stop(true);
}

void
Modulator::stop(bool const quickClose)
{
  m_quickClose = quickClose;
  close();
}

void
Modulator::close()
{
  if (m_stream)
  {
    if (m_quickClose) m_stream->reset();
    else              m_stream->stop();
  }

  m_state.store(State::Idle);
  AudioDevice::close();
}

qint64
Modulator::readData(char * const data,
                    qint64 const maxSize)
{
  if (maxSize == 0) return 0;

  Q_ASSERT (!(maxSize % qint64(bytesPerFrame()))); // no torn frames
  Q_ASSERT (isOpen());

  qint64               framesGenerated = 0;
  qint64         const maxFrames       = maxSize / bytesPerFrame();
  qint16       *       samples         = reinterpret_cast<qint16 *>(data);
  qint16 const * const samplesEnd      = samples + maxFrames * (bytesPerFrame() / sizeof(qint16));

  switch (m_state.load())
  {
    case State::Synchronizing:
    {
      if (m_silentFrames)
      {
        // Send silence up to end of start delay.

        framesGenerated = qMin(m_silentFrames, maxFrames);

        do
        {
          samples = load(0, samples);
        }
        while (--m_silentFrames && samples != samplesEnd);

        if (!m_silentFrames)
        {
          m_state.store(State::Active);
        }
      }
    }
    [[fallthrough]];

    case State::Active:
    {
      // Fade out parameters; no fade out during tuning.

      unsigned int const i0 = (m_tuning ? 9999 : (JS8_NUM_SYMBOLS - 0.017) * 4.0) * m_nsps;
      unsigned int const i1 = (m_tuning ? 9999 :  JS8_NUM_SYMBOLS          * 4.0) * m_nsps;

      while (samples != samplesEnd && m_ic < i1)
      {
        unsigned int const isym = m_tuning ? 0 : m_ic / (4.0 * m_nsps);

        if (isym != m_isym0 || m_audioFrequency != m_audioFrequency0)
        {
          double const toneFrequency = m_audioFrequency + itone[isym] * m_toneSpacing;

          m_dphi       = TAU * toneFrequency / FRAME_RATE;
          m_isym0      = isym;
          m_audioFrequency0 = m_audioFrequency;
        }

        m_phi += m_dphi;

        if (m_phi > TAU) m_phi -= TAU;
        if (m_ic  > i0)  m_amp  = 0.98 * m_amp;
        if (m_ic  > i1)  m_amp  = 0.0;

        samples = load(qRound(m_amp * qSin(m_phi)), samples);

        ++framesGenerated;
        ++m_ic;
      }

      if (m_amp == 0.0)
      {
        m_state.store(State::Idle);
        return framesGenerated * bytesPerFrame();
        m_phi = 0.0;
      }

      m_audioFrequency0 = m_audioFrequency;

      // Done for this chunk; continue on the next call. Pad the
      // block with silence.

      while (samples != samplesEnd)
      {
        samples = load(0, samples);
        ++framesGenerated;
      }

      return framesGenerated * bytesPerFrame();
    }
    [[fallthrough]];

    case State::Idle:
    break;
  }

  Q_ASSERT (isIdle());
  return 0;
}

Q_LOGGING_CATEGORY(modulator_js8, "modulator.js8", QtWarningMsg)
