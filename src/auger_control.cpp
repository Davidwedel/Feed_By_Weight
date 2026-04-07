#include "auger_control.h"
#include "config.h"
#include "types.h"

extern SystemStatus systemStatus;
extern Config config;

AugerControl::AugerControl() {
    _augerRunning = false;
    _chainRunning = false;
    _stage = FeedingStage::STOPPED;
    _targetWeight = 0;
    _startWeight = 0;
    _weightDispensed = 0;
	_projectedWeightDispensed = 0;
	_learnedProjectedWeightDispensed = 0;
    _feedStartTime = 0;
    _chainStartTime = 0;
    _bothRunningStartTime = 0;
    _lastWeightCheck = 0;
    _postAveragingStartTime = 0;
    _alarmTriggered = false;
    _chainPreRunTime = 10;
    _maxRuntime = 600;
    _alarmThreshold = 10.0;
    _lastValidWeight = 0;
    _weightReadingFailed = false;
    _warningPending = false;
    _warnedWeightFail = false;
    _warnedIncrease = false;
    _warnedLowRate = false;
    _stageBeforePause = FeedingStage::STOPPED;
    _weightWhenPaused = 0;
    strcpy(_alarmReason, "");
    strcpy(_warningMessage, "");
}

void AugerControl::begin() {
    // Configure relay pins as outputs
    pinMode(RELAY_1_PIN, OUTPUT);
    pinMode(RELAY_2_PIN, OUTPUT);
    pinMode(RELAY_3_PIN, OUTPUT);
    pinMode(RELAY_4_PIN, OUTPUT);
    pinMode(RELAY_5_PIN, OUTPUT);
    pinMode(RELAY_6_PIN, OUTPUT);

    // Ensure all relays are OFF at startup
    stopAll();

    Serial.println("Auger and chain control initialized");
}

/**
 * START FEEDING SEQUENCE
 *
 * Initiates a 2-stage feeding sequence:
 * 1. Chain runs alone for chainPreRunTime seconds (moves feed toward auger)
 * 2. Both auger + chain run together until targetWeight is dispensed
 *
 * Also configures:
 * - Safety monitoring (low flow rate warnings, max runtime alarm)
 * - Warning flags (reset for new feeding cycle)
 */
void AugerControl::startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot start feeding - already in progress");
        return;
    }

    // Store feeding parameters
    _targetWeight = targetWeight;
    _chainPreRunTime = chainPreRunTime;
    _maxRuntime = maxRuntime;

    // Initialize timing
    _feedStartTime = millis();
    _chainStartTime = millis();
    _lastWeightCheck = millis();

    // Calculate start weight as average of all available readings from history
	if (_startWeight == 0)
    	_startWeight = getAveragedWeight();
    Serial.printf("Start weight: %.2f lbs (averaged from %d samples)\n", _startWeight,
                  systemStatus.historyCount >= 5 ? systemStatus.historyCount : 1);

    _weightDispensed = 0;
	_projectedWeightDispensed = 0;

    // Reset all warning/alarm flags for new feeding cycle
    _alarmTriggered = false;
    _warningPending = false;
    _warnedWeightFail = false;
    _warnedLowRate = false;
    strcpy(_alarmReason, "");

    // Stage 1: Start chain only (auger stays off)
    _stage = FeedingStage::CHAIN_ONLY;
    Serial.println("About to start chain...");

	//make sure that bins aren't getting filled before we actually start the auger
	if(systemStatus.binFillDetected) {
		pauseFeeding(false); // byUser
		return;
	}
    Serial.printf("Feeding started: Target=%.2f, ChainPreRun=%ds, MaxTime=%ds\n",
                  targetWeight, chainPreRunTime, maxRuntime);

    controlChain(true);

}

/**
 * UPDATE FEEDING STATE MACHINE
 *
 * Called every loop (~100Hz) from main.cpp while in FEEDING state.
 * Manages the feeding sequence, monitors safety conditions, detects bin fills.
 *
 * State flow:
 * CHAIN_ONLY → (timer) → BOTH_RUNNING → (target reached) → POST_AVERAGING → (60s) → COMPLETED
 *                                     → (max runtime) → FAILED
 *                                     → (bin fill detected) → PAUSED → resume
 *
 * Returns current stage so main.cpp can handle completion/failure.
 */
FeedingStage AugerControl::update(float currentTotalWeight) {

	//restart if we paused and bin fill has ended
	if (_stage == FeedingStage::PAUSED && !_pausedByUser && !systemStatus.binFillDetected){
		resumeFeeding(false); // byUser
	}

    // Don't update if not in an active feeding stage
    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED
        || _stage == FeedingStage::FAILED || _stage == FeedingStage::PAUSED
        || _stage == FeedingStage::TERMINATED) {
        return _stage;
    }

	//we gotta stop!
	//Post avging handles it differently
	if (_stage != FeedingStage::POST_AVERAGING && systemStatus.binFillDetected)
	{
		pauseFeeding(false); // byUser
	}

    // Weight Reading Validation
    // BinTrac read failures show as 0 or negative. Use last valid weight to continue.
    if (currentTotalWeight <= 0) {
        if (!_warnedWeightFail) {
            sendWarning("Weight reading failed - continuing until max runtime");
            _warnedWeightFail = true;
        }
        _weightReadingFailed = true;
        // Fall back to last valid weight if available
        if (_lastValidWeight > 0) {
            currentTotalWeight = _lastValidWeight;
        }
    } else {
        // Weight reading restored
        if (_weightReadingFailed && _warnedWeightFail) {
            sendWarning("Weight reading restored");
            _warnedWeightFail = false;
        }
        _weightReadingFailed = false;
        _lastValidWeight = currentTotalWeight;
    }

    // Calculate weight dispensed (bins get lighter as feed goes out)
    _weightDispensed = _startWeight - systemStatus.totalCurrentWeight;
    _projectedWeightDispensed = _weightDispensed + config.projectedWeight;

    unsigned long elapsed = (millis() - _feedStartTime) / 1000;  // Total elapsed time in seconds

    // Feeding Stage State Machine
    switch (_stage) {
        case FeedingStage::CHAIN_ONLY:
            // Stage 1: Chain running alone (auger still off)
            if ((millis() - _chainStartTime) / 1000 >= _chainPreRunTime) {
                // Pre-run time elapsed - start auger too
                Serial.printf("Chain pre-run complete (%ds), starting auger...\n", _chainPreRunTime);
                controlAuger(true);
                _stage = FeedingStage::BOTH_RUNNING;

                // Reset timing for safety monitoring (start fresh from when both running)
                _bothRunningStartTime = millis();

                Serial.println("Stage: BOTH_RUNNING");
            }
            break;

        case FeedingStage::BOTH_RUNNING:
            {
            // Stage 2: Both auger + chain running
            // Main feeding stage. Monitor weight dispensed and check for completion.

            // Check if target weight reached (SUCCESS CONDITION)
            if (_projectedWeightDispensed >= _targetWeight) {
                stopAll();
                _lastWeightCheck = millis();  // Record when motors stopped
                _postAveragingStartTime = millis();
                _stage = FeedingStage::POST_AVERAGING;
                Serial.printf("Target reached: Projected Dispensed=%.2f, Raw Dispensed=%.2f, in %lus. Starting post-averaging (60s)...\n",
                             _projectedWeightDispensed, _weightDispensed, elapsed);
                return _stage;
            }

            // Low Feed Rate Warning
            // Uses flow rate calculated from weight history (oldest to newest).
            // If dispensing less than threshold lb/min, warn but don't stop.
            // Wait at least 60 seconds after auger starts before checking.
            unsigned long augerElapsed = (millis() - _bothRunningStartTime) / 1000;
            if (augerElapsed >= 60 && systemStatus.historyCount >= SystemStatus::WEIGHT_HISTORY_SIZE) {
                if (systemStatus.flowRate < _alarmThreshold) {
                    if (!_warnedLowRate) {
                        sendWarning("Low feed rate - bin may be empty or jammed");
                        _warnedLowRate = true;
                    }
                } else if (_warnedLowRate) {
                    // Feed rate improved - clear warning
                    sendWarning("Feed rate normal");
                    _warnedLowRate = false;
                }
            }

            // Maximum Runtime Check (FAILURE CONDITION)
            // Only hard failure condition. Prevents infinite feeding if something
            // goes wrong (broken scale, jammed auger, etc.)
            if (elapsed >= _maxRuntime) {
                triggerAlarm("Maximum runtime exceeded");
            }
            }
            break;

        case FeedingStage::POST_AVERAGING:
            // Post-Averaging: Motors stopped, waiting for weight to settle
            // After target weight reached, wait 60 seconds for bins to settle,
            // then use averaged weight for final dispensed calculation.
            // Continue monitoring for bin fills - if detected, use oldest reading.

            {
                // Check if bin fill started during settling period
                if (systemStatus.binFillDetected) {
                    // Bin fill detected during settling - use oldest available reading
                    int oldestIdx = (systemStatus.historyIndex - systemStatus.historyCount + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
                    float endWeight = systemStatus.weightHistory[oldestIdx];
                    _weightDispensed = _startWeight - endWeight;
					_projectedWeightDispensed = _weightDispensed + config.projectedWeight;
                    _stage = FeedingStage::COMPLETED;
                    Serial.printf("Bin fill detected during post-averaging. Using oldest reading. Final dispensed: %.2f lbs\n",
                                 _projectedWeightDispensed);
                    return _stage;
                }

                unsigned long postElapsed = (millis() - _postAveragingStartTime) / 1000;

                // Check if 60 seconds elapsed
                if (postElapsed >= 90) {
                    // Average all available readings for final weight
                    float endWeight = getAveragedWeight();

					//save start weight for next feeding
					_startWeight = endWeight;

                    int samplesToAverage = systemStatus.historyCount >= 5 ? systemStatus.historyCount : 1;

					float _uncalcedDispensed = _weightDispensed;

                    _weightDispensed = _startWeight - endWeight;

					float rawLearnedProjected = _weightDispensed - _uncalcedDispensed;

					//all the same for right now.
					_projectedWeightDispensed = _weightDispensed;
                    _stage = FeedingStage::COMPLETED;
                    Serial.printf("Post-averaging complete (averaged %d samples). Final dispensed: %.2f lbs\n",
                                 samplesToAverage, _weightDispensed);

					Serial.printf("raw learned projected = %.2f\n", rawLearnedProjected);
                    return _stage;
                }
            }
            break;

        default:
            break;
    }

    return _stage;
}

void AugerControl::stopAll() {
    controlAuger(false);
    controlChain(false);
    _stage = FeedingStage::STOPPED;
}

/**
 * MANUAL PAUSE/RESUME
 *
 * Allows user to pause feeding via web UI (also used by pause for bin fill, see bool byUser).
 * Remembers which stage we were in and resumes there.
 */
void AugerControl::pauseFeeding(bool byUser) {
    if (_stage != FeedingStage::CHAIN_ONLY && _stage != FeedingStage::BOTH_RUNNING) {
        Serial.println("Cannot pause - not actively feeding");
        return;
    }

	//use projected because it'll be closer--everything is guessing tho
	_weightWhenPaused = systemStatus.projectedWeightDispensed;
    _stageBeforePause = _stage;  // Remember where we were
								 
	_pausedByUser = byUser;//make so only user can unpause if paused and vice versa
						   
    controlAuger(false);
    controlChain(false);
    _stage = FeedingStage::PAUSED;
    Serial.printf("Feeding paused: Projected Dispensed=%.2f, Raw Dispensed=%.2f\n",
                 _projectedWeightDispensed, _weightDispensed);
}

void AugerControl::resumeFeeding(bool byUser) {
	if (byUser != _pausedByUser) return;  
										 
    if (_stage != FeedingStage::PAUSED) {
        Serial.println("Cannot resume - not paused");
        return;
    }

    // Adjust _startWeight to account for any weight added during pause (bin filling)
    // Use averaged weight for stable reference after bins settle
    float currentWeight = getAveragedWeight();
    float weightAddedDuringPause = currentWeight - _weightWhenPaused;
    _startWeight += weightAddedDuringPause;
    Serial.printf("Resume: weight changed by %.2f lbs during pause, adjusted start weight to %.2f\n",
                  weightAddedDuringPause, _startWeight);

    // Restore previous stage
    _stage = _stageBeforePause;

    // Restart motors appropriate for that stage
    if (_stage == FeedingStage::CHAIN_ONLY) {
        controlChain(true);
    } else if (_stage == FeedingStage::BOTH_RUNNING) {
        controlChain(true);
        controlAuger(true);
        // Reset monitoring timers
        _bothRunningStartTime = millis();
    }

    Serial.printf("Feeding resumed to %s: Projected Dispensed=%.2f, Raw Dispensed=%.2f\n",
                  _stage == FeedingStage::CHAIN_ONLY ? "CHAIN_ONLY" : "BOTH_RUNNING",
                  _projectedWeightDispensed, _weightDispensed);
}

void AugerControl::terminate() {
    controlAuger(false);
    controlChain(false);
    _lastWeightCheck = millis();
    _stage = FeedingStage::TERMINATED;
    Serial.printf("Feeding terminated by user: Projected Dispensed=%.2f, Raw Dispensed=%.2f\n",
                 _projectedWeightDispensed, _weightDispensed);
}

float AugerControl::getAveragedWeight(int samplesToAverage) {
    // Return single reading if insufficient history
    if (systemStatus.historyCount < 5) {
        return systemStatus.totalCurrentWeight;
    }

    // Default to all available samples
    if (samplesToAverage < 0 || samplesToAverage > systemStatus.historyCount) {
        samplesToAverage = systemStatus.historyCount;
    }

    // Average the requested number of samples from history
    float avg = 0;
    for (int i = 0; i < samplesToAverage; i++) {
        int idx = (systemStatus.historyIndex - 1 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
        avg += systemStatus.weightHistory[idx];
    }
    return avg / samplesToAverage;
}

void AugerControl::triggerAlarm(const char* reason) {
    if (_alarmTriggered) return;  // Already triggered

    _alarmTriggered = true;
    strncpy(_alarmReason, reason, sizeof(_alarmReason) - 1);
    _alarmReason[sizeof(_alarmReason) - 1] = '\0';

    Serial.printf("ALARM: %s\n", reason);

    // Stop all motors immediately
    controlAuger(false);
    controlChain(false);
    _lastWeightCheck = millis();
    _stage = FeedingStage::FAILED;
}

void AugerControl::sendWarning(const char* warning) {
    strncpy(_warningMessage, warning, sizeof(_warningMessage) - 1);
    _warningMessage[sizeof(_warningMessage) - 1] = '\0';
    _warningPending = true;
    Serial.printf("WARNING: %s\n", warning);
}

unsigned long AugerControl::getDuration() const {
    if (_feedStartTime == 0) return 0;

    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED
        || _stage == FeedingStage::FAILED || _stage == FeedingStage::TERMINATED
        || _stage == FeedingStage::POST_AVERAGING) {
        return (_lastWeightCheck - _feedStartTime) / 1000;
    }

    return (millis() - _feedStartTime) / 1000;
}

void AugerControl::setAuger(bool state) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot manual control - feeding in progress");
        return;
    }
    controlAuger(state);
}

void AugerControl::setChain(bool state) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot manual control - feeding in progress");
        return;
    }
    controlChain(state);
}

/**
 * LOW-LEVEL RELAY CONTROL
 *
 * These functions actually control the GPIO pins connected to relay modules.
 * Each relay controls a motor contactor.
 *
 * Auger: 2 relays (RELAY_1_PIN, RELAY_6_PIN) - both controlled together
 * Chain: 4 relays (RELAY_2_PIN through RELAY_5_PIN) - one per chain feeder line
 */
void AugerControl::controlAuger(bool state) {
    digitalWrite(RELAY_1_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_6_PIN, state ? HIGH : LOW);
    _augerRunning = state;
    Serial.printf("GPIOs %d,%d (Augers): %s\n", RELAY_1_PIN, RELAY_6_PIN, state ? "ON (HIGH)" : "OFF (LOW)");
}

void AugerControl::controlChain(bool state) {
    digitalWrite(RELAY_2_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_3_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_4_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_5_PIN, state ? HIGH : LOW);
    _chainRunning = state;
    Serial.printf("GPIOs %d,%d,%d,%d (Chains A-D): %s\n", RELAY_2_PIN, RELAY_3_PIN, RELAY_4_PIN, RELAY_5_PIN, state ? "ON (HIGH)" : "OFF (LOW)");
}
