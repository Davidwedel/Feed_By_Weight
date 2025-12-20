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
    void startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime, float fillDetectionThreshold = 20.0, uint16_t fillSettlingTime = 60);

    // Stop all immediately
    void stopAll();

    // Update - call frequently in main loop
    // Returns current feeding stage
    FeedingStage update(float currentTotalWeight);

    // Get status
    bool isAugerRunning() const { return _augerRunning; }
    bool isChainRunning() const { return _chainRunning; }
    FeedingStage getStage() const { return _stage; }
    float getWeightDispensed() const { return _weightDispensed; }
    float getFlowRate() const;  // lbs/min
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

    // Check if feeding is active (only active stages, not terminal states)
    bool isFeeding() const {
        return _stage == FeedingStage::CHAIN_ONLY || _stage == FeedingStage::BOTH_RUNNING;
    }

private:
    bool _augerRunning;
    bool _chainRunning;
    FeedingStage _stage;

    float _targetWeight;
    float _startWeight;
    float _weightDispensed;
    float _alarmThreshold;
    float _fillDetectionThreshold;

    uint16_t _chainPreRunTime;  // How long chain runs alone (seconds)
    uint16_t _maxRuntime;
    uint16_t _fillSettlingTime;  // How long to wait after bin fill stabilizes

    unsigned long _feedStartTime;
    unsigned long _chainStartTime;
    unsigned long _bothRunningStartTime;
    unsigned long _lastWeightCheck;

    bool _alarmTriggered;
    char _alarmReason[64];
    char _warningMessage[128];
    bool _warningPending;

    // Weight change tracking for warnings
    float _weightAtMinuteStart;
    unsigned long _minuteStartTime;
    float _lastValidWeight;
    bool _weightReadingFailed;

    // Track which warnings have been sent (once per cycle)
    bool _warnedWeightFail;
    bool _warnedNoChange;
    bool _warnedIncrease;
    bool _warnedLowRate;

    // Bin filling detection and pause state
    FeedingStage _stageBeforePause;
    float _lastWeight;                // Previous weight reading for fill detection
    float _weightWhenPaused;          // Weight at the moment pause triggered (never changes)
    float _lastWeightDuringPause;     // Last seen weight while monitoring (updates during pause)
    unsigned long _fillStabilizedTime;
    bool _fillInProgress;

    // Safety and warnings
    void checkSafety(float currentWeight);
    void triggerAlarm(const char* reason);
    void sendWarning(const char* warning);

    // Low-level relay control
    void controlAuger(bool state);
    void controlChain(bool state);
};

#endif // AUGER_CONTROL_H
