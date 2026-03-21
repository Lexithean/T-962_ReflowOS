#ifndef REFLOW_H_
#define REFLOW_H_

typedef enum eReflowMode {
	REFLOW_INITIAL=0,
	REFLOW_STANDBY,
	REFLOW_BAKE,
	REFLOW_REFLOW,
	REFLOW_STANDBYFAN,
	REFLOW_BBTUNE
} ReflowMode_t;

#define SETPOINT_MIN (30)
#define SETPOINT_MAX (300)
#define SETPOINT_DEFAULT (30)
#define TOTAL_DOTS	110

#define BBTUNE_TARGET_HIGH (200)
#define BBTUNE_TARGET_LOW  (150)
#define BBTUNE_NUM_CYCLES  (3)

// Bang-bang tune phases
typedef enum eBBTunePhase {
	BBTUNE_PROMPT = 0,
	BBTUNE_HEATING,
	BBTUNE_HEAT_COAST,
	BBTUNE_COOLING,
	BBTUNE_COOL_COAST,
	BBTUNE_DONE
} BBTunePhase_t;

// 36 hours max timer
#define BAKE_TIMER_MAX (60 * 60 * 36)

void Reflow_Init(void);
void Reflow_TogglePause(void);
int Reflow_IsPaused(void);
void Reflow_SetMode(ReflowMode_t themode);
void Reflow_SetSetpoint(uint16_t thesetpoint);
void Reflow_LoadSetpoint(void);
int16_t Reflow_GetActualTemp(void);
uint8_t Reflow_IsDone(void);
int Reflow_IsPreheating(void);
uint16_t Reflow_GetSetpoint(void);
void Reflow_SetBakeTimer(int seconds);
int Reflow_GetTimeLeft(void);
int32_t Reflow_Run(uint32_t thetime, float meastemp, uint8_t* pheat, uint8_t* pfan, int32_t manualsetpoint);
void Reflow_ToggleStandbyLogging(void);
void Reflow_PlotDots(void);

// Bang-bang auto-tune
void Reflow_BBTune_Start(void);
void Reflow_BBTune_Stop(void);
BBTunePhase_t Reflow_BBTune_GetPhase(void);
int Reflow_BBTune_GetCycle(void);
int Reflow_BBTune_GetHeatOffset(void);
int Reflow_BBTune_GetCoolOffset(void);
int32_t Reflow_BBTune_Work(uint8_t* pheat, uint8_t* pfan);

#endif /* REFLOW_H_ */
