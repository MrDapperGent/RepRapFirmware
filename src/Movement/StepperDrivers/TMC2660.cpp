/*
 * TMC2660.cpp
 *
 *  Created on: 23 Jan 2016
 *      Author: David
 */

#include "RepRapFirmware.h"

#if SUPPORT_TMC2660

#include "TMC2660.h"
#include "RepRap.h"
#include "Movement/Move.h"
#include "sam/drivers/pdc/pdc.h"
#include "sam/drivers/usart/usart.h"

const float MaximumMotorCurrent = 2400.0;
const uint32_t DefaultMicrosteppingShift = 4;				// x16 microstepping
const bool DefaultInterpolation = true;						// interpolation enabled
const int DefaultStallDetectThreshold = 1;
const bool DefaultStallDetectFiltered = false;
const unsigned int DefaultMinimumStepsPerSecond = 200;		// for stall detection: 1 rev per second assuming 1.8deg/step, as per the TMC2660 datasheet

static size_t numTmc2660Drivers;

static bool driversPowered = false;

const int ChopperControlRegisterMode = 999;					// mode passed to get/set microstepping to indicate we want the chopper control register

// The SPI clock speed is a compromise:
// - too high and polling the driver chips take too much of the CPU time
// - too low and we won't detect stalls quickly enough
// With a 4MHz SPI clock:
// - polling the drivers makes calculations take 13.5% longer, so it is taking about 12% of the CPU time
// - we poll all 10 drivers in about 80us
// With a 2MHz SPI clock:
// - polling the drivers makes calculations take 8.3% longer, so it is taking about 7.7% of the CPU time
// - we poll all 10 drivers in about 170us
const uint32_t DriversSpiClockFrequency = 2000000;			// 2MHz SPI clock

const int DefaultStallGuardThreshold = 1;					// Range is -64..63. Zero seems to be too sensitive. Higher values reduce sensitivity of stall detection.

// TMC2660 register addresses
const uint32_t TMC_REG_DRVCTRL = 0;
const uint32_t TMC_REG_CHOPCONF = 0x80000;
const uint32_t TMC_REG_SMARTEN = 0xA0000;
const uint32_t TMC_REG_SGCSCONF = 0xC0000;
const uint32_t TMC_REG_DRVCONF = 0xE0000;
const uint32_t TMC_DATA_MASK = 0x0001FFFF;

// DRVCONF register bits
const uint32_t TMC_DRVCONF_RDSEL_0 = 0 << 4;
const uint32_t TMC_DRVCONF_RDSEL_1 = 1 << 4;
const uint32_t TMC_DRVCONF_RDSEL_2 = 2 << 4;
const uint32_t TMC_DRVCONF_RDSEL_3 = 3 << 4;
const uint32_t TMC_DRVCONF_VSENSE = 1 << 6;
const uint32_t TMC_DRVCONF_SDOFF = 1 << 7;
const uint32_t TMC_DRVCONF_TS2G_3P2 = 0 << 8;
const uint32_t TMC_DRVCONF_TS2G_1P6 = 1 << 8;
const uint32_t TMC_DRVCONF_TS2G_1P2 = 2 << 8;
const uint32_t TMC_DRVCONF_TS2G_0P8 = 3 << 8;
const uint32_t TMC_DRVCONF_DISS2G = 1 << 10;
const uint32_t TMC_DRVCONF_SLPL_MIN = 0 << 12;
const uint32_t TMC_DRVCONF_SLPL_MED = 2 << 12;
const uint32_t TMC_DRVCONF_SLPL_MAX = 3 << 12;
const uint32_t TMC_DRVCONF_SLPH_MIN = 0 << 14;
const uint32_t TMC_DRVCONF_SLPH_MIN_TCOMP = 1 << 14;
const uint32_t TMC_DRVCONF_SLPH_MED_TCOMP = 2 << 14;
const uint32_t TMC_DRVCONF_SLPH_MAX = 3 << 14;
const uint32_t TMC_DRVCONF_TST = 1 << 16;

// Chopper control register bits
const uint32_t TMC_CHOPCONF_TOFF_MASK = 15;
const uint32_t TMC_CHOPCONF_TOFF_SHIFT = 0;
#define TMC_CHOPCONF_TOFF(n)	((((uint32_t)n) & 15) << 0)
#define TMC_CHOPCONF_HSTRT(n)	((((uint32_t)n) & 7) << 4)
#define TMC_CHOPCONF_HEND(n)	((((uint32_t)n) & 15) << 7)
#define TMC_CHOPCONF_HDEC(n)	((((uint32_t)n) & 3) << 11)
const uint32_t TMC_CHOPCONF_RNDTF = 1 << 13;
const uint32_t TMC_CHOPCONF_CHM = 1 << 14;
const uint32_t TMC_CHOPCONF_TBL_MASK = (3 << 15);
const uint32_t TMC_CHOPCONF_TBL_SHIFT = 15;
#define TMC_CHOPCONF_TBL(n)	(((uint32_t)n & 3) << 15)

// Driver control register bits, when SDOFF=0
const uint32_t TMC_DRVCTRL_MRES_MASK = 0x0F;
const uint32_t TMC_DRVCTRL_MRES_SHIFT = 0;
const uint32_t TMC_DRVCTRL_MRES_16 = 0x04;
const uint32_t TMC_DRVCTRL_MRES_32 = 0x03;
const uint32_t TMC_DRVCTRL_MRES_64 = 0x02;
const uint32_t TMC_DRVCTRL_MRES_128 = 0x01;
const uint32_t TMC_DRVCTRL_MRES_256 = 0x00;
const uint32_t TMC_DRVCTRL_DEDGE = 1 << 8;
const uint32_t TMC_DRVCTRL_INTPOL = 1 << 9;

// stallGuard2 control register
const uint32_t TMC_SGCSCONF_CS_MASK = 31;
#define TMC_SGCSCONF_CS(n) ((((uint32_t)n) & 31) << 0)
const uint32_t TMC_SGCSCONF_SGT_MASK = 127 << 8;
const uint32_t TMC_SGCSCONF_SGT_SHIFT = 8;
#define TMC_SGCSCONF_SGT(n) ((((uint32_t)n) & 127) << 8)
const uint32_t TMC_SGCSCONF_SGT_SFILT = 1 << 16;

// coolStep control register
const uint32_t TMC_SMARTEN_SEMIN_MASK = 15;
const uint32_t TMC_SMARTEN_SEMIN_SHIFT = 0;
const uint32_t TMC_SMARTEN_SEUP_1 = 0 << 5;
const uint32_t TMC_SMARTEN_SEUP_2 = 1 << 5;
const uint32_t TMC_SMARTEN_SEUP_4 = 2 << 5;
const uint32_t TMC_SMARTEN_SEUP_8 = 3 << 5;
const uint32_t TMC_SMARTEN_SEMAX_MASK = 15;
const uint32_t TMC_SMARTEN_SEMAX_SHIFT = 8;
const uint32_t TMC_SMARTEN_SEDN_32 = 0 << 13;
const uint32_t TMC_SMARTEN_SEDN_8 = 1 << 13;
const uint32_t TMC_SMARTEN_SEDN_2 = 2 << 13;
const uint32_t TMC_SMARTEN_SEDN_1 = 3 << 13;
const uint32_t TMC_SMARTEN_SEIMIN_HALF = 0 << 15;
const uint32_t TMC_SMARTEN_SEIMIN_QTR = 1 << 15;

const unsigned int NumWriteRegisters = 5;

// Chopper control register defaults
// 0x901B4 as per datasheet example
// CHM bit not set, so uses spread cycle mode
const uint32_t defaultChopConfReg =
	  TMC_REG_CHOPCONF
	| TMC_CHOPCONF_TBL(2)				// blanking time 36 clocks which is about 2.4us typical (should maybe use 16 or 24 instead?)
	| TMC_CHOPCONF_HDEC(0)				// no hysteresis decrement
	| TMC_CHOPCONF_HEND(3)				// HEND = 0
	| TMC_CHOPCONF_HSTRT(3)				// HSTRT = 4
	| TMC_CHOPCONF_TOFF(4);				// TOFF = 9.2us

// StallGuard configuration register
const uint32_t defaultSgscConfReg =
	  TMC_REG_SGCSCONF
	| TMC_SGCSCONF_SGT(DefaultStallGuardThreshold);

// Driver configuration register
const uint32_t defaultDrvConfReg =
	  TMC_REG_DRVCONF
	| TMC_DRVCONF_RDSEL_1				// read SG register in status
	| TMC_DRVCONF_VSENSE				// use high sensitivity range
	| TMC_DRVCONF_TS2G_0P8				// fast short-to-ground detection
	| 0;

// Driver control register
const uint32_t defaultDrvCtrlReg =
	  TMC_REG_DRVCTRL
	| TMC_DRVCTRL_MRES_16
	| TMC_DRVCTRL_INTPOL;				// x16 microstepping with interpolation

// coolStep control register
const uint32_t defaultSmartEnReg =
	  TMC_REG_SMARTEN
	| 0;								// disable coolStep, it needs to be tuned ot the motor to work properly

//----------------------------------------------------------------------------------------------------------------------------------
// Private types and methods

class TmcDriverState
{
public:
	void Init(uint32_t p_axisNumber, uint32_t p_pin);
	void SetAxisNumber(size_t p_axisNumber);
	void WriteAll();

	bool SetChopConf(uint32_t newVal);
	bool SetOffTime(uint32_t newVal);
	uint32_t GetChopConf() const;
	uint32_t GetOffTime() const;
	void SetCoolStep(uint16_t coolStepConfig);
	bool SetMicrostepping(uint32_t shift, bool interpolate);
	unsigned int GetMicrostepping(bool& interpolation) const;		// Get microstepping
	bool SetDriverMode(unsigned int mode);
	DriverMode GetDriverMode() const;
	void SetCurrent(float current);
	void Enable(bool en);
	void UpdateChopConfRegister();
	void SetStallDetectThreshold(int sgThreshold);
	void SetStallDetectFilter(bool sgFilter);
	void SetStallMinimumStepsPerSecond(unsigned int stepsPerSecond);
	void AppendStallConfig(const StringRef& reply) const;
	void AppendDriverStatus(const StringRef& reply);

	void TransferDone() __attribute__ ((hot));						// called by the ISR when the SPI transfer has completed
	void StartTransfer() __attribute__ ((hot));						// called to start a transfer

	uint32_t ReadLiveStatus() const;
	uint32_t ReadAccumulatedStatus(uint32_t bitsToKeep);

private:
	void ResetLoadRegisters()
	{
		minSgLoadRegister = 1023;
		maxSgLoadRegister = 0;
	}

	static void SetupDMA(uint32_t outVal) __attribute__ ((hot));	// set up the PDC to send a register and receive the status

	static constexpr unsigned int NumRegisters = 5;			// the number of registers that we write to
	volatile uint32_t registers[NumRegisters];				// the values we want the TMC2660 writable registers to have

	// Register numbers are in priority order, most urgent first
	static constexpr unsigned int DriveControl = 0;			// microstepping
	static constexpr unsigned int StallGuardConfig = 1;		// motor current and stall threshold
	static constexpr unsigned int ChopperControl = 2;		// enable/disable
	static constexpr unsigned int DriveConfig = 3;			// read register select, sense voltage high/low sensitivity
	static constexpr unsigned int SmartEnable = 4;			// coolstep configuration

	static constexpr uint32_t UpdateAllRegisters = (1u << NumRegisters) - 1;	// bitmap in registersToUpdate for all registers

	uint32_t pin;											// the pin number that drives the chip select pin of this driver
	uint32_t configuredChopConfReg;							// the configured chopper control register, in the Enabled state
	volatile uint32_t registersToUpdate;					// bitmap of register values that need to be sent to the driver chip
	uint32_t axisNumber;									// the axis number of this driver as used to index the DriveMovements in the DDA
	uint32_t microstepShiftFactor;							// how much we need to shift 1 left by to get the current microstepping
	uint32_t maxStallStepInterval;							// maximum interval between full steps to take any notice of stall detection
	uint32_t minSgLoadRegister;								// the minimum value of the StallGuard bits we read
	uint32_t maxSgLoadRegister;								// the maximum value of the StallGuard bits we read

	volatile uint32_t lastReadStatus;						// the status word that we read most recently, updated by the ISR
	volatile uint32_t accumulatedStatus;
	bool enabled;
};

// State structures for all drivers
static TmcDriverState driverStates[MaxSmartDrivers];

// PDC address
static Pdc * const spiPdc =
#if TMC2660_USES_USART
			usart_get_pdc_base(USART_TMC2660);
#else
			spi_get_pdc_base(SPI_TMC2660);
#endif

// Words to send and receive driver SPI data from/to
volatile static uint32_t spiDataOut = 0;					// volatile because we care about when it is written
volatile static uint32_t spiDataIn = 0;						// volatile because the PDC writes it

// Variables used by the ISR
static TmcDriverState * volatile currentDriver = nullptr;	// volatile because the ISR changes it

// Set up the PDC to send a register and receive the status
/*static*/ inline void TmcDriverState::SetupDMA(uint32_t outVal)
{
	// Faster code, not using the ASF
	spiPdc->PERIPH_PTCR = (PERIPH_PTCR_RXTDIS | PERIPH_PTCR_TXTDIS);		// disable the PDC

	// SPI sends data MSB first, but the firmware uses little-endian mode, so we need to reverse the byte order
	spiDataOut = cpu_to_be32(outVal << 8);

	spiPdc->PERIPH_TPR = reinterpret_cast<uint32_t>(&spiDataOut);
	spiPdc->PERIPH_TCR = 3;

	spiPdc->PERIPH_RPR = reinterpret_cast<uint32_t>(&spiDataIn);
	spiPdc->PERIPH_RCR = 3;

	spiPdc->PERIPH_PTCR = (PERIPH_PTCR_RXTEN | PERIPH_PTCR_TXTEN);			// enable the PDC
}

// Initialise the state of the driver and its CS pin
void TmcDriverState::Init(uint32_t p_axisNumber, uint32_t p_pin)
pre(!driversPowered)
{
	axisNumber = p_axisNumber;
	pin = p_pin;
	pinMode(pin, OUTPUT_HIGH);
	enabled = false;
	registers[DriveControl] = defaultDrvCtrlReg;
	configuredChopConfReg = defaultChopConfReg;
	registers[ChopperControl] = configuredChopConfReg & ~TMC_CHOPCONF_TOFF_MASK;		// disable driver at startup
	registers[SmartEnable] = defaultSmartEnReg;
	registers[StallGuardConfig] = defaultSgscConfReg;
	registers[DriveConfig] = defaultDrvConfReg;
	registersToUpdate = UpdateAllRegisters;
	accumulatedStatus = lastReadStatus = 0;
	ResetLoadRegisters();
	SetMicrostepping(DefaultMicrosteppingShift, DefaultInterpolation);
	SetStallDetectThreshold(DefaultStallDetectThreshold);
	SetStallDetectFilter(DefaultStallDetectFiltered);
	SetStallMinimumStepsPerSecond(DefaultMinimumStepsPerSecond);
}

inline void TmcDriverState::SetAxisNumber(size_t p_axisNumber)
{
	axisNumber = p_axisNumber;
}

// Write all registers. This is called when the drivers are known to be powered up.
inline void TmcDriverState::WriteAll()
{
	registersToUpdate = UpdateAllRegisters;
}

// Check the new chopper control register, update it and return true if it is legal
bool TmcDriverState::SetChopConf(uint32_t newVal)
{
	// TOFF = 0 turns the driver off so it is not allowed.
	// TOFF = 1 is not allowed if TBL = 0.
	const uint32_t toff = (newVal & TMC_CHOPCONF_TOFF_MASK) >> TMC_CHOPCONF_TOFF_SHIFT;
	if (toff == 0 || (toff == 1 && ((newVal & TMC_CHOPCONF_TBL_MASK) == 0)))
	{
		return false;
	}
	configuredChopConfReg = (newVal & 0x0001FFFF) | TMC_REG_CHOPCONF;		// save the new value
	UpdateChopConfRegister();												// send the new value, keeping the current Enable status
	return true;
}

// Set the off time in the chopper control register
bool TmcDriverState::SetOffTime(uint32_t newVal)
{
	if (newVal > 15)
	{
		return false;
	}
	return SetChopConf((configuredChopConfReg & ~TMC_CHOPCONF_TOFF_MASK) | ((newVal << TMC_CHOPCONF_TOFF_SHIFT) & TMC_CHOPCONF_TOFF_MASK));
}

// Set the driver mode
bool TmcDriverState::SetDriverMode(unsigned int mode)
{
	switch (mode)
	{
	case (unsigned int)DriverMode::constantOffTime:
		return SetChopConf((configuredChopConfReg & ~TMC_CHOPCONF_RNDTF) | TMC_CHOPCONF_CHM);

	case (unsigned int)DriverMode::randomOffTime:
		return SetChopConf(configuredChopConfReg | TMC_CHOPCONF_RNDTF | TMC_CHOPCONF_CHM);

	case (unsigned int)DriverMode::spreadCycle:
		return SetChopConf(configuredChopConfReg & ~(TMC_CHOPCONF_RNDTF | TMC_CHOPCONF_CHM));

	default:
		return false;
	}
}

// Get the driver mode
DriverMode TmcDriverState::GetDriverMode() const
{
	return ((configuredChopConfReg & TMC_CHOPCONF_CHM) == 0) ? DriverMode::spreadCycle
			: ((configuredChopConfReg & TMC_CHOPCONF_RNDTF) == 0) ? DriverMode::constantOffTime
				: DriverMode::randomOffTime;
}

// Set the microstepping and microstep interpolation. The desired microstepping is (1 << shift).
bool TmcDriverState::SetMicrostepping(uint32_t shift, bool interpolate)
{
	microstepShiftFactor = shift;
	uint32_t drvCtrlReg = registers[DriveControl] & ~TMC_DRVCTRL_MRES_MASK;
	drvCtrlReg |= (((8u - shift) << TMC_DRVCTRL_MRES_SHIFT) & TMC_DRVCTRL_MRES_MASK);
	if (interpolate)
	{
		drvCtrlReg |= TMC_DRVCTRL_INTPOL;
	}
	else
	{
		drvCtrlReg &= ~TMC_DRVCTRL_INTPOL;
	}
	registers[DriveControl] = drvCtrlReg;
	registersToUpdate |= 1u << DriveControl;
	return true;
}

// Set the motor current
void TmcDriverState::SetCurrent(float current)
{
	// The current sense resistor on the production Duet WiFi is 0.051 ohms.
	// This gives us a range of 101mA to 3.236A in 101mA steps in the high sensitivity range (VSENSE = 1)
	const uint32_t iCurrent = static_cast<uint32_t>(constrain<float>(current, 100.0, MaximumMotorCurrent));
	const uint32_t csBits = (32 * iCurrent - 1600)/3236;		// formula checked by simulation on a spreadsheet
	registers[StallGuardConfig] = (registers[StallGuardConfig] & ~TMC_SGCSCONF_CS_MASK) | TMC_SGCSCONF_CS(csBits);
	registersToUpdate |= 1u << StallGuardConfig;
}

// Enable or disable the driver. Also called from SetChopConf after the chopper control configuration has been changed.
void TmcDriverState::Enable(bool en)
{
	if (enabled != en)
	{
		if (en)
		{
			// Driver was disabled and we are enabling it, so clear the stall status
			// Unfortunately this may not be sufficient, because the stall status probably won't be updated until the next full step position.
			accumulatedStatus &= ~TMC_RR_SG;
			lastReadStatus &= ~TMC_RR_SG;
		}
		enabled = en;
		UpdateChopConfRegister();
	}
}

void TmcDriverState::UpdateChopConfRegister()
{
	registers[ChopperControl] = (enabled) ? configuredChopConfReg : (configuredChopConfReg & ~TMC_CHOPCONF_TOFF_MASK);
	registersToUpdate |= 1u << ChopperControl;
}

// Read the status
inline uint32_t TmcDriverState::ReadLiveStatus() const
{
	const uint32_t ret = lastReadStatus & (TMC_RR_SG | TMC_RR_OT | TMC_RR_OTPW | TMC_RR_S2G | TMC_RR_OLA | TMC_RR_OLB | TMC_RR_STST);
	return (enabled) ? ret : ret & ~TMC_RR_SG;
}

// Read the status
uint32_t TmcDriverState::ReadAccumulatedStatus(uint32_t bitsToKeep)
{
	const uint32_t mask = (enabled) ? 0xFFFFFFFF : ~TMC_RR_SG;
	bitsToKeep &= mask;
	const irqflags_t flags = cpu_irq_save();
	const uint32_t status = accumulatedStatus;
	accumulatedStatus &= bitsToKeep;
	cpu_irq_restore(flags);
	return status & (TMC_RR_SG | TMC_RR_OT | TMC_RR_OTPW | TMC_RR_S2G | TMC_RR_OLA | TMC_RR_OLB | TMC_RR_STST) & mask;
}

void TmcDriverState::SetStallDetectThreshold(int sgThreshold)
{
	const uint32_t sgVal = ((uint32_t)constrain<int>(sgThreshold, -64, 63)) & 127;
	registers[StallGuardConfig] = (registers[StallGuardConfig] & ~TMC_SGCSCONF_SGT_MASK) | (sgVal << TMC_SGCSCONF_SGT_SHIFT);
	registersToUpdate |= 1u << StallGuardConfig;
}

void TmcDriverState::SetStallDetectFilter(bool sgFilter)
{
	if (sgFilter)
	{
		registers[StallGuardConfig] |= TMC_SGCSCONF_SGT_SFILT;
	}
	else
	{
		registers[StallGuardConfig] &= ~TMC_SGCSCONF_SGT_SFILT;
	}
	registersToUpdate |= 1u << StallGuardConfig;
}

void TmcDriverState::SetStallMinimumStepsPerSecond(unsigned int stepsPerSecond)
{
	maxStallStepInterval = StepClockRate/max<unsigned int>(stepsPerSecond, 1);
}

void TmcDriverState::SetCoolStep(uint16_t coolStepConfig)
{
	registers[SmartEnable] = TMC_REG_SMARTEN | coolStepConfig;
	registersToUpdate |= 1u << SmartEnable;
}

void TmcDriverState::AppendStallConfig(const StringRef& reply) const
{
	const bool filtered = ((registers[StallGuardConfig] & TMC_SGCSCONF_SGT_SFILT) != 0);
	int threshold = (int)((registers[StallGuardConfig] & TMC_SGCSCONF_SGT_MASK) >> TMC_SGCSCONF_SGT_SHIFT);
	if (threshold >= 64)
	{
		threshold -= 128;
	}
	reply.catf("stall threshold %d, filter %s, steps/sec %" PRIu32 ", coolstep %" PRIx32,
				threshold, ((filtered) ? "on" : "off"), StepClockRate/maxStallStepInterval, registers[SmartEnable] & 0xFFFF);
}

// Append the driver status to a string, and reset the min/max load values
void TmcDriverState::AppendDriverStatus(const StringRef& reply)
{
	if (lastReadStatus & TMC_RR_OT)
	{
		reply.cat(" temperature-shutdown!");
	}
	else if (lastReadStatus & TMC_RR_OTPW)
	{
		reply.cat(" temperature-warning");
	}
	if (lastReadStatus & TMC_RR_S2G)
	{
		reply.cat(" short-to-ground");
	}
	if ((lastReadStatus & TMC_RR_OLA) && !(lastReadStatus & TMC_RR_STST))
	{
		reply.cat(" open-load-A");
	}
	if ((lastReadStatus & TMC_RR_OLB) && !(lastReadStatus & TMC_RR_STST))
	{
		reply.cat(" open-load-B");
	}
	if (lastReadStatus & TMC_RR_STST)
	{
		reply.cat(" standstill");
	}
	else if ((lastReadStatus & (TMC_RR_OT | TMC_RR_OTPW | TMC_RR_S2G | TMC_RR_OLA | TMC_RR_OLB)) == 0)
	{
		reply.cat(" ok");
	}
	
	if (minSgLoadRegister <= maxSgLoadRegister)
	{
		reply.catf(", SG min/max %" PRIu32 "/%" PRIu32, minSgLoadRegister, maxSgLoadRegister);
	}
	else
	{
		reply.cat(", SG min/max not available");
	}
	ResetLoadRegisters();
}

// Get microstepping
unsigned int TmcDriverState::GetMicrostepping(bool& interpolation) const
{
	interpolation = (registers[DriveControl] & TMC_DRVCTRL_INTPOL) != 0;
	return 1u << microstepShiftFactor;
}

// Get chopper control register
uint32_t TmcDriverState::GetChopConf() const
{
	return configuredChopConfReg & TMC_DATA_MASK;
}

// Get the off time from the chopper control register
uint32_t TmcDriverState::GetOffTime() const
{
	return (configuredChopConfReg & TMC_CHOPCONF_TOFF_MASK) >> TMC_CHOPCONF_TOFF_SHIFT;
}

// This is called by the ISR when the SPI transfer has completed
inline void TmcDriverState::TransferDone()
{
	fastDigitalWriteHigh(pin);									// set the CS pin high for the driver we just polled
	if (driversPowered)											// if the power is still good, update the status
	{
		uint32_t status = be32_to_cpu(spiDataIn) >> 12;			// get the status
		const uint32_t interval = reprap.GetMove().GetStepInterval(axisNumber, microstepShiftFactor);		// get the full step interval
		if (interval == 0 || interval > maxStallStepInterval)	// if the motor speed is too low to get reliable stall indication
		{
			status &= ~TMC_RR_SG;								// remove the stall status bit
		}
		else
		{
			const uint32_t sgLoad = (status >> TMC_RR_SG_LOAD_SHIFT) & 1023;	// get the StallGuard load register
			if (sgLoad < minSgLoadRegister)
			{
				minSgLoadRegister = sgLoad;
			}
			if (sgLoad > maxSgLoadRegister)
			{
				maxSgLoadRegister = sgLoad;
			}
		}
		lastReadStatus = status;
		accumulatedStatus |= status;
	}
}

// This is called from the ISR or elsewhere to start a new SPI transfer. Inlined for ISR speed.
inline void TmcDriverState::StartTransfer()
{
	currentDriver = this;

	// Find which register to send. The common case is when no registers need to be updated.
	uint32_t regVal;
	if (registersToUpdate == 0)
	{
		regVal = registers[SmartEnable];
	}
	else
	{
		size_t regNum = 0;
		uint32_t mask = 1;
		do
		{
			if ((registersToUpdate & mask) != 0)
			{
				break;
			}
			++regNum;
			mask <<= 1;
		} while (regNum < NumRegisters - 1);
		registersToUpdate &= ~mask;
		regVal = registers[regNum];
	}

	// Kick off a transfer for that register
	const irqflags_t flags = cpu_irq_save();			// avoid race condition

#if TMC2660_USES_USART
	USART_TMC2660->US_CR = US_CR_RSTRX | US_CR_RSTTX;	// reset transmitter and receiver
#else
	SPI_TMC2660->SPI_CR = SPI_CR_SPIDIS;				// disable the SPI
	(void)SPI_TMC2660->SPI_RDR;							// clear the receive buffer
#endif

	fastDigitalWriteLow(pin);							// set CS low
	SetupDMA(regVal);									// set up the PDC

#if TMC2660_USES_USART
	USART_TMC2660->US_IER = US_IER_ENDRX;				// enable end-of-transfer interrupt
	USART_TMC2660->US_CR = US_CR_RXEN | US_CR_TXEN;		// enable transmitter and receiver
#else
	SPI_TMC2660->SPI_IER = SPI_IER_ENDRX;				// enable end-of-transfer interrupt
	SPI_TMC2660->SPI_CR = SPI_CR_SPIEN;					// enable SPI
#endif

	cpu_irq_restore(flags);
}

// ISR for the USART

#ifndef TMC2660_SPI_Handler
# error TMC handler name not defined
#endif

extern "C" void TMC2660_SPI_Handler(void) __attribute__ ((hot));

void TMC2660_SPI_Handler(void)
{
	TmcDriverState *driver = currentDriver;				// capture volatile variable
	if (driver != nullptr)
	{
		driver->TransferDone();							// tidy up after the transfer we just completed
		if (driversPowered)
		{
			// Power is still good, so send/receive to/from the next driver
			++driver;									// advance to the next driver
			if (driver == driverStates + numTmc2660Drivers)
			{
				driver = driverStates;
			}
			driver->StartTransfer();
			return;
		}
	}

	// Driver power is down or there is no current driver, so stop polling

#if TMC2660_USES_USART
	USART_TMC2660->US_IDR = US_IDR_ENDRX;
#else
	SPI_TMC2660->SPI_IDR = SPI_IDR_ENDRX;
#endif

	currentDriver = nullptr;							// signal that we are not waiting for an interrupt
}

//--------------------------- Public interface ---------------------------------

namespace SmartDrivers
{
	// Initialise the driver interface and the drivers, leaving each drive disabled.
	// It is assumed that the drivers are not powered, so driversPowered(true) must be called after calling this before the motors can be moved.
	void Init(const Pin driverSelectPins[DRIVES], size_t numTmcDrivers)
	{
		numTmc2660Drivers = min<size_t>(numTmcDrivers, MaxSmartDrivers);

		// Make sure the ENN pins are high
		pinMode(GlobalTmc2660EnablePin, OUTPUT_HIGH);

		// The pins are already set up for SPI in the pins table
		ConfigurePin(GetPinDescription(TMC2660MosiPin));
		ConfigurePin(GetPinDescription(TMC2660MisoPin));
		ConfigurePin(GetPinDescription(TMC2660SclkPin));

		// Enable the clock to the USART or SPI
		pmc_enable_periph_clk(ID_TMC2660_SPI);

#if TMC2660_USES_USART
		// Set USART_EXT_DRV in SPI mode, with data changing on the falling edge of the clock and captured on the rising edge
		USART_TMC2660->US_IDR = ~0u;
		USART_TMC2660->US_CR = US_CR_RSTRX | US_CR_RSTTX | US_CR_RXDIS | US_CR_TXDIS;
		USART_TMC2660->US_MR = US_MR_USART_MODE_SPI_MASTER
						| US_MR_USCLKS_MCK
						| US_MR_CHRL_8_BIT
						| US_MR_CHMODE_NORMAL
						| US_MR_CPOL
						| US_MR_CLKO;
		USART_TMC2660->US_BRGR = VARIANT_MCK/DriversSpiClockFrequency;		// set SPI clock frequency
		USART_TMC2660->US_CR = US_CR_RSTRX | US_CR_RSTTX | US_CR_RXDIS | US_CR_TXDIS | US_CR_RSTSTA;

		// We need a few microseconds of delay here for the USART to sort itself out before we send any data,
		// otherwise the processor generates two short reset pulses on its own NRST pin, and resets itself.
		// 2016-07-07: removed this delay, because we no longer send commands to the TMC2660 drivers immediately.
		//delay(10);
#else
		// Set up the SPI interface with data changing on the falling edge of the clock and captured on the rising edge
		spi_reset(SPI_TMC2660);										// this clears the transmit and receive registers and puts the SPI into slave mode
		SPI_TMC2660->SPI_MR = SPI_MR_MSTR							// master mode
						| SPI_MR_MODFDIS							// disable fault detection
						| SPI_MR_PCS(0);							// fixed peripheral select

		// Set SPI mode, clock frequency, CS active after transfer, delay between transfers
		const uint16_t baud_div = (uint16_t)spi_calc_baudrate_div(DriversSpiClockFrequency, SystemCoreClock);
		const uint32_t csr = SPI_CSR_SCBR(baud_div)					// Baud rate
						| SPI_CSR_BITS_8_BIT						// Transfer bit width
						| SPI_CSR_DLYBCT(0)      					// Transfer delay
						| SPI_CSR_CSAAT								// Keep CS low after transfer in case we are slow in writing the next byte
						| SPI_CSR_CPOL;								// clock high between transfers
		SPI_TMC2660->SPI_CSR[0] = csr;
#endif

		driversPowered = false;
		for (size_t driver = 0; driver < numTmc2660Drivers; ++driver)
		{
			driverStates[driver].Init(driver, driverSelectPins[driver]);		// axes are mapped straight through to drivers initially
		}
	}

	void SetAxisNumber(size_t driver, uint32_t axisNumber)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].SetAxisNumber(axisNumber);
		}
	}

	void SetCurrent(size_t driver, float current)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].SetCurrent(current);
		}
	}

	void EnableDrive(size_t driver, bool en)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].Enable(en);
		}
	}

	uint32_t GetLiveStatus(size_t driver)
	{
		return (driver < numTmc2660Drivers) ? driverStates[driver].ReadLiveStatus() : 0;
	}

	uint32_t GetAccumulatedStatus(size_t driver, uint32_t bitsToKeep)
	{
		return (driver < numTmc2660Drivers) ? driverStates[driver].ReadAccumulatedStatus(bitsToKeep) : 0;
	}

	// Set microstepping and microstep interpolation
	bool SetMicrostepping(size_t driver, unsigned int microsteps, bool interpolate)
	{
		if (driver < numTmc2660Drivers && microsteps > 0)
		{
			// Set the microstepping. We need to determine how many bits right to shift the desired microstepping to reach 1.
			unsigned int shift = 0;
			unsigned int uSteps = (unsigned int)microsteps;
			while ((uSteps & 1) == 0)
			{
				uSteps >>= 1;
				++shift;
			}
			if (uSteps == 1 && shift <= 8)
			{
				driverStates[driver].SetMicrostepping(shift, interpolate);
				return true;
			}
		}
		return false;
	}

	// Get microstepping and interpolation
	unsigned int GetMicrostepping(size_t driver, bool& interpolation)
	{
		if (driver < numTmc2660Drivers)
		{
			return driverStates[driver].GetMicrostepping(interpolation);
		}
		interpolation = false;
		return 1;
	}

	bool SetDriverMode(size_t driver, unsigned int mode)
	{
		return driver < numTmc2660Drivers && driverStates[driver].SetDriverMode(mode);
	}

	DriverMode GetDriverMode(size_t driver)
	{
		return (driver < numTmc2660Drivers) ? driverStates[driver].GetDriverMode() : DriverMode::unknown;
	}

	bool SetChopperControlRegister(size_t driver, uint32_t ccr)
	{
		return driver < numTmc2660Drivers && driverStates[driver].SetChopConf(ccr);
	}

	uint32_t GetChopperControlRegister(size_t driver)
	{
		return (driver < numTmc2660Drivers) ? driverStates[driver].GetChopConf() : 0;
	}

	bool SetOffTime(size_t driver, uint32_t offTime)
	{
		return driver < numTmc2660Drivers && driverStates[driver].SetOffTime(offTime);
	}

	uint32_t GetOffTime(size_t driver)
	{
		return (driver < numTmc2660Drivers) ? driverStates[driver].GetOffTime() : 0;
	}

	// Flag the the drivers have been powered up.
	// Before the first call to this function with powered true, you must call Init().
	void Spin(bool powered)
	{
		const bool wasPowered = driversPowered;
		driversPowered = powered;
		if (powered)
		{
			if (!wasPowered)
			{
				// Power to the drivers has been provided or restored, so we need to enable and re-initialise them
				digitalWrite(GlobalTmc2660EnablePin, LOW);
				delayMicroseconds(10);

				for (size_t driver = 0; driver < numTmc2660Drivers; ++driver)
				{
					driverStates[driver].WriteAll();
				}
			}
			if (currentDriver == nullptr && numTmc2660Drivers != 0)
			{
				// Kick off the first transfer
				NVIC_EnableIRQ(TMC2660_SPI_IRQn);
				driverStates[0].StartTransfer();
			}
		}
		else if (wasPowered)
		{
			digitalWrite(GlobalTmc2660EnablePin, HIGH);			// disable the drivers
		}
	}

	// This is called from the tick ISR, possibly while Spin (with powered either true or false) is being executed
	void TurnDriversOff()
	{
		digitalWrite(GlobalTmc2660EnablePin, HIGH);				// disable the drivers
		driversPowered = false;
	}

	void SetStallThreshold(size_t driver, int sgThreshold)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].SetStallDetectThreshold(sgThreshold);
		}
	}

	void SetStallFilter(size_t driver, bool sgFilter)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].SetStallDetectFilter(sgFilter);
		}
	}

	void SetStallMinimumStepsPerSecond(size_t driver, unsigned int stepsPerSecond)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].SetStallMinimumStepsPerSecond(stepsPerSecond);
		}
	}

	void SetCoolStep(size_t drive, uint16_t coolStepConfig)
	{
		if (drive < numTmc2660Drivers)
		{
			driverStates[drive].SetCoolStep(coolStepConfig);
		}
	}

	void AppendStallConfig(size_t driver, const StringRef& reply)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].AppendStallConfig(reply);
		}
	}

	void AppendDriverStatus(size_t driver, const StringRef& reply)
	{
		if (driver < numTmc2660Drivers)
		{
			driverStates[driver].AppendDriverStatus(reply);
		}
	}

	float GetStandstillCurrentPercent(size_t driver)
	{
		return 100.0;			// not supported
	}

	void SetStandstillCurrentPercent(size_t driver, float percent)
	{
		// not supported so nothing to see here
	}

};	// end namespace

#endif

// End




