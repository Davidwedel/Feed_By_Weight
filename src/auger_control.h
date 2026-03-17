#ifndef AUGER_CONTROL_H
#define AUGER_CONTROL_H

#include <Arduino.h>
#include "types.h"

class AugerControl {
public:
    AugerControl();

    // Initialize relay pins
    void begin();

    // Start feeding cycle
    void startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime, uint16_t fillSettlingMinutes = 1);

    // Stop all immediately
    void stopAll();

    // Pause/resume manual control during feeding
    void pauseFeeding();
    void resumeFeeding();
    void terminate();

    // Update - call frequently in main loop
    // Returns current feeding stage
    FeedingStage update(float currentTotalWeight);

    // Get status
    bool isAugerRunning() const { return _augerRunning; }
    bool isChainRunning() const { return _chainRunning; }
    FeedingStage getStage() const { return _stage; }
    float getWeightDispensed() const { return _weightDispensed; }
    unsigned long getDuration() const;
    bool isAlarmTriggered() const { return _alarmTriggered; }
    const char* getAlarmReason() const { return _alarmReason; }

    // Get warning (if any) - returns new warnings only
    const char* getNewWarning() {
        if (_warningPending) {
            _warningPending = false;
            return _warningMessage;
        }
        return nullptr;
    }

    // Manual control
    void setAuger(bool state);
    void setChain(bool state);

    // Check if feeding is active (including paused states, not terminal states)
    bool isFeeding() const {
        return _stage == FeedingStage::CHAIN_ONLY || _stage == FeedingStage::BOTH_RUNNING
            || _stage == FeedingStage::PAUSED_FOR_FILL || _stage == FeedingStage::PAUSED_MANUAL
            || _stage == FeedingStage::POST_AVERAGING;
    }

private:
    bool _augerRunning;
    bool _chainRunning;
    FeedingStage _stage;

    float _targetWeight;
    float _startWeight;
    float _weightDispensed;
    float _alarmThreshold;

    uint16_t _chainPreRunTime;  // How long chain runs alone (seconds)
    uint16_t _maxRuntime;
    uint16_t _fillSettlingTime;  // How long to wait after bin fill stabilizes

    unsigned long _feedStartTime;
    unsigned long _chainStartTime;
    unsigned long _bothRunningStartTime;
    unsigned long _lastWeightCheck;
    unsigned long _postAveragingStartTime;

    bool _alarmTriggered;
    char _alarmReason[64];
    char _warningMessage[128];
    bool _warningPending;

    // Weight change tracking for warnings
    float _lastValidWeight;
    bool _weightReadingFailed;

    // Track which warnings have been sent (once per cycle)
    bool _warnedWeightFail;
    bool _warnedIncrease;
    bool _warnedLowRate;

    // Bin filling detection and pause state
    FeedingStage _stageBeforePause;
    float _lastWeight;                // Previous weight reading for fill detection
    float _fillRateWeight;            // Weight at start of rate evaluation window
    unsigned long _fillRateStartTime; // Timestamp of rate evaluation window start
    float _weightWhenPaused;          // Weight at the moment pause triggered (never changes)
    float _lastWeightDuringPause;     // Last seen weight while monitoring (updates during pause)
    unsigned long _fillStabilizedTime;
    bool _fillInProgress;

    // Safety and warnings
    void triggerAlarm(const char* reason);
    void sendWarning(const char* warning);

    // Low-level relay control
    void controlAuger(bool state);
    void controlChain(bool state);
};

#endif // AUGER_CONTROL_H
