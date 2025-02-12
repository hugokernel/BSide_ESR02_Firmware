/* ************************************************************************
 *
 *   capacitor measurements
 *
 *   (c) 2012-2023 by Markus Reschke
 *   based on code from Markus Frejek and Karl-Heinz K�bbeler
 *
 * ************************************************************************ */


/*
 *  local constants
 */

/* source management */
#define CAP_C


/*
 *  include header files
 */

/* local includes */
#include "config.h"           /* global configuration */
#include "common.h"           /* common header file */
#include "variables.h"        /* global variables */
#include "functions.h"        /* external functions */


/* ************************************************************************
 *   ESR measurement
 * ************************************************************************ */


#ifdef SW_ESR

/*
 *  set up timer for delay
 *  - uses Timer0 as MCU cycle timer
 *
 *  requires:
 *  - number of MCU cycles
 *
 *  returns:
 *  - 1 on success
 *  - 0 on error
 */

uint8_t SetUpDelayTimer(uint8_t Cycles)
{
  uint8_t           Flag = 0;      /* return value */

  /*
   *  adjust cycles for DelayTimer():
   *  - -4 for function call
   *  - -3 for starting timer
   *  - -2 for waiting loop
   *  - -4 for stopping timer
   *  - -4 for return
   *  - total: 17
   */

  #define CYCLE_OFFSET   17

  if (Cycles < CYCLE_OFFSET) return Flag;    /* check for required minimum */
  Cycles -= CYCLE_OFFSET;                    /* substract cycles for DelayTimer() */ 

  #undef CYCLE_OFFSET


  /*
   *  set up Timer0:
   *  - CTC mode (count up to OCR0A) 
   *  - prescaler 1 to match MCU cycles
   */

  TCCR0B = 0;                      /* stop timer */
  TCCR0A = (1 << WGM01);           /* set CTC mode, disable output compare pins */
  OCR0A = Cycles;                  /* set number of MCU cycles */
  /* todo: check if we have to substract one cycle for setting the flag */

  Flag = 1;              /* signal success */
  return Flag;
}



/*
 *  start timer and wait for timeout
 *  (requires prior call of SetUpPulseTimer() for setup)
 */

void DelayTimer(void)
{
  TCNT0 = 0;                       /* reset counter to 0 */
  TCCR0B = (1 << CS00);            /* start timer by setting prescaler */

  while (!(TIFR0 & (1 << OCF0A)));   /* wait for output compare A match flag */

  TCCR0B = 0;                      /* stop timer */
  TIFR0 = (1 << OCF0A);            /* clear flag */
}



/*
 *  measure ESR
 *  - tolerates charge up to about 130mV
 *
 *  requires:
 *  - pointer to cap data structure
 *
 *  returns:
 *  - ESR in 0.01 Ohm
 *  - UINT16_MAX on any problem
 */

uint16_t MeasureESR(Capacitor_Type *Cap)
{
  uint16_t          ESR = UINT16_MAX;   /* return value */
  uint16_t          U_1;           /* voltage at probe 1 with pos. pulse unloaded */
  uint16_t          U_2;           /* voltage at probe 2 with pos. pulse loaded */
  uint16_t          U_3;           /* voltage at probe 2 with neg. pulse unloaded */
  uint16_t          U_4;           /* voltage at probe 1 with neg. pulse loaded */
  uint8_t           Probe1;        /* probe #1 */
  uint8_t           Probe2;        /* probe #2 */
  uint8_t           Bits;          /* register bits for ADC */
  uint8_t           n;             /* counter */
  uint32_t          Sum_1;         /* sum #1 */
  uint32_t          Sum_2;         /* sum #2 */
  uint32_t          Value;

  /* check for a capacitor >= 10nF */
  if ((Cap == NULL) ||
      (CmpValue(Cap->Value, Cap->Scale, 10, -9) < 0)) return ESR;

  /*
   *  Hint: 
   *  - When we would use MilliSleep() we'd have to change the MCU
   *    sleep mode to idle to keep the timer running in sleep mode.
   */


  /*
   *  init stuff
   */

  DischargeProbes();                    /* try to discharge probes */
  if (Check.Found == COMP_ERROR) return ESR;   /* skip on error */

  /* Some testers need additional discharging to lower the cap's residual
     voltage to a reasonable level. */
  DischargeCap(Cap->A, Cap->B);         /* additional discharge */

  UpdateProbes2(Cap->A, Cap->B);        /* update probes */
  Probe1 = Probes.Ch_1;                 /* ADC MUX for probe-1 */
  Probe2 = Probes.Ch_2;                 /* ADC MUX for probe-2 */

  Probe1 |= ADC_REF_BANDGAP;            /* select bandgap reference */
  Probe2 |= ADC_REF_BANDGAP;            /* select bandgap reference */

  /* register bits to enable and start ADC */
  Bits = (1 << ADSC) | (1 << ADEN) | (1 << ADIF) | ADC_CLOCK_DIV;

  /* init variables */
  Sum_1 = 1;             /* 1 to prevent division by zero */
  Sum_2 = 1;             /* 1 to prevent division by zero */


  /*
   *  We have to create a delay to shift the middle of the current pulse to
   *  the ADC's S&H. S&H happens at 1.5 ADC clock cycles after starting the
   *  conversion. We synchronize to a dummy conversion done directly before,
   *  so we have 2.5 ADC clock cycles to S&H. The required delay between the
   *  dummy conversion and S&H of the next conversion is:
   *    MCU cycles for 2.5 ADC clock cycles
   *    - MCU cycles for waiting loop for completion of dummy conversion (4)
   *    - MCU cycles for starting next conversion (2)
   *    - MCU cycles for fixed pre-delay of 10�s
   *    - MCU cycles for enabling pulse (4)
   *    - MCU cycles for half-pulse (2�s)
   *
   *  MCU     ADC      MCU cycles for        
   *  clock   clock    2.5 ADC cycles  pre-delay   half-pulse  delay
   *  ---------------------------------------------------------------
   *   8 MHz  125 kHz   160 (20�s)      80 (10�s)  16 (2�s)     54
   *                                               32 (4�s)     38
   *  16 MHz  125 kHz   320 (20�s)     160 (10�s)  32 (2�s)    118
   *                                               64 (4�s)     86
   *  20 MHz  156.25    320 (16�s)     200 (10�s)  40 (2�s)     70
   *                                               80 (4�s)     30
   *  
   *  Skipping the second half-pulse allows us to measure low value caps too.
   */

  /* delay for pulse */
  /* MCU cycles for one ADC cycle * 2.5 - MCU cycles for 10�s 
     - MCU cycles for half-pulse - 10 */
  U_1 = ((MCU_CYCLES_PER_ADC * 25) / 10) - (MCU_CYCLES_PER_US * 10)
        - (MCU_CYCLES_PER_US * 2) - 10;
  #if CPU_FREQ == 8000000
  /* magic time shift to compensate missing second half-pulse */
  U_1 -= 4;
  #endif
  n = (uint8_t)U_1;

  /* set up delay timer */
  if (SetUpDelayTimer(n) == 0) return ESR;   /* skip on error */


  /*
   *  measurement loop:
   *  - simulate AC by short positive and negative pulses
   *  - measure start voltage without DUT
   *  - measure pulse voltage with DUT
   *  - pre-charge DUT with a negative pulse of half length to
   *    to compensate voltage rise by positive charging pulse
   *  - 16 & 20 MHz MCUs seem to measure higher ESR values
   */  

  ADC_PORT = 0;          /* set ADC port to low */
  ADMUX = Probe1;        /* set input channel to probe-1 & set bandgap ref */
  wait10ms();            /* time for voltage stabilization */

  U_2 = 50;              /* don't start with positive half-pulse */
  U_4 = 0;               /* start with a negative half-pulse */
  n = 255;               /* set loop counter */

  while (n > 0)
  {
    wdt_reset();                   /* reset watchdog */

    /*
     *  mitigate runaway of cap's charge/voltage
     */

    if (U_4 < 50)             /* voltage too low for last negative pulse */
    {
      /* charge cap a little bit more: negative pulse */

      /* set probes: GND -- probe-2 / probe-1 -- Rl -- Vcc */
      ADC_DDR = Probes.Pin_2;      /* pull down probe-2 directly */
      R_PORT = Probes.Rl_1;        /* pull up probe-1 via Rl */
      R_DDR = Probes.Rl_1;         /* enable pull up */
      wait2us();                   /* wait half-pulse */
      R_DDR = 0;                   /* disable any pull up */      
      R_PORT = 0;                  /* reset probe resistors */
    }

    if (U_2 < 50)             /* voltage too low for last positive pulse */
    {
      /* charge cap a little bit more: positive pulse */

      /* set probes: GND -- probe-1 / probe-2 -- Rl -- Vcc */
      ADC_DDR = Probes.Pin_1;      /* pull down probe-1 directly */
      R_PORT = Probes.Rl_2;        /* pull up probe-2 via Rl */
      R_DDR = Probes.Rl_2;         /* enable pull up */
      wait2us();                   /* wait half-pulse */
      R_DDR = 0;                   /* disable any pull up */      
      R_PORT = 0;                  /* reset probe resistors */
    }


    /*
     *  forward mode, probe-1 only (probe-2 in HiZ mode)
     *  set probes: GND -- probe-1 -- Rl -- Vcc / probe-2 -- HiZ
     *  get voltage at probe-1 (voltage at RiL)
     */

    ADC_DDR = Probes.Pin_1;        /* pull down probe-1 directly to GND */
    R_PORT = Probes.Rl_1;          /* pull up probe-1 via Rl */
    R_DDR = Probes.Rl_1;           /* enable resistor */
    ADMUX = Probe1;                /* set input channel to probe-1 & set bandgap ref */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    /* real conversion */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_1 = ADCW;                    /* save ADC value */


    /*
     *  forward mode, positive charging pulse
     *  set probes: GND -- probe-1 / probe-2 -- Rl -- Vcc
     *  get voltage at probe-2 (voltage at DUT, i.e. RiL + ESR)
     */

    ADMUX = Probe2;                /* set input channel to probe-2 & set bandgap ref */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */

    /* read ADC in the mid of a positive charging pulse */
    ADCSRA = Bits;                 /* start conversion with next ADC clock cycle */
    wait10us();                    /* fixed pre-delay */
    DelayTimer();                  /* delay for pulse */
    R_PORT = Probes.Rl_2;          /* pull up probe-2 via Rl */
    R_DDR = Probes.Rl_2;           /* enable resistor */
    wait2us();                     /* first half-pulse */
                                   /* S/H happens here */
    #if CPU_FREQ < 8000000
    wait2us();                     /* second half-pulse */
    #endif
//    R_PORT = 0;                    /* set resistor port to low */
    R_DDR = 0;                     /* set resistor port to HiZ */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_2 = ADCW;                    /* save ADC value */


    /*
     *  reverse mode, probe-2 only (probe-1 in HiZ mode)
     *  set probes: GND -- probe-2 -- Rl -- Vcc / probe-1 -- HiZ
     *  get voltage at probe-2 (voltage at RiL)
     */

    ADC_DDR = Probes.Pin_2;        /* pull down probe-2 directly */
    R_PORT = Probes.Rl_2;          /* pull up probe-2 via Rl */
    R_DDR = Probes.Rl_2;           /* enable resistor */
    ADMUX = Probe2;                /* set input channel to probe-2 & set bandgap ref */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    /* real conversion */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_3 = ADCW;                    /* save ADC value */


    /*
     *  reverse mode, negative charging pulse
     *  set probes: GND -- probe-2 / probe-1 -- Rl -- Vcc
     *  get voltage at probe-1 (voltage at DUT, i.e. RiL + ESR)
     */

    ADMUX = Probe1;                /* set input channel to probe-1 & set bandgap ref */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */

    /* read ADC in the mid of a negatve charging pulse */
    ADCSRA = Bits;                 /* start conversion with next ADC clock cycle */
    wait10us();                    /* fixed pre-delay */
    DelayTimer();                  /* delay for pulse */
    R_PORT = Probes.Rl_1;          /* pull up probe-1 via Rl */
    R_DDR = Probes.Rl_1;           /* enable resistor */
    wait2us();                     /* first half-pulse */
                                   /* S/H happens here */
    #if CPU_FREQ < 8000000
    wait2us();                     /* second half-pulse */
    #endif
//    R_PORT = 0;                    /* set resistor port to low */
    R_DDR = 0;                     /* set resistor port to HiZ */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_4 = ADCW;                    /* save ADC value */


    /*
     *  manage measured values
     */

    Sum_1 += U_1;        /* positive pulse without DUT */
    Sum_1 += U_3;        /* negative pulse without DUT */
    Sum_2 += U_2;        /* positive pulse with DUT */
    Sum_2 += U_4;        /* negative pulse with DUT */
    n--;                 /* next loop run */
  }

  /* probes: reset to safe mode */
  ADC_DDR = 0;
  R_PORT = 0;


  /*
   *  process measurements
   */

  if (Sum_2 > Sum_1)               /* valid measurement */
  {
    /*
     *  calculate ESR
     *  - ESR = U_ESR / I_ESR
     *    with U_ESR = (U2 or U4) and I_ESR = (U1 or U3) / RiL
     *    ESR = (U2 or U4) * RiL / (U1 or U3)
     *  - since we divide (U2 or U4) by (U1 or U3), we don't need to convert
     *    the ADC value into a voltage and simply desample the sums.
     *  - so ESR = Sum_2 * RiL / Sum_1
     *  - for a resolution of 0.01 Ohms we have to scale RiL to 0.01 Ohms
     */

    /* voltage across the DUT (raw value) */
    Sum_2 -= Sum_1;           /* subtract voltage at DUT's low side (RiL) */

    /* ESR = Sum_2 * RiL / Sum_1 */
    Value = (uint32_t)(NV.RiL * 10);    /* RiL in 0.01 Ohms */
    Value *= Sum_2;                     /* sum of raw values for voltage across DUT */
    Value /= Sum_1;                     /* sum of raw values for voltage at RiL */
    U_1 = (uint16_t)Value;              /* raw ESR (0.01 Ohms) */

    /* consider probe resistance */
    #ifdef R_MULTIOFFSET
      /* get index number for probe pair */
      n = GetOffsetIndex(Probes.ID_1, Probes.ID_2);
      U_2 = NV.RZero[n];
    #else
      U_2 = NV.RZero;
    #endif

    if (U_1 > U_2)            /* larger than offset */
    {
      U_1 -= U_2;             /* subtract offset */
      ESR = U_1;              /* got result */
    }
    else                      /* offset problem or zero */
    {
      /* should only happen for large caps (>1000�F) */
      if (CmpValue(Cap->Value, Cap->Scale, 1000, -6) > 0)
      {
        ESR = 0;                   /* can't be less than 0 Ohms */
      }
    }
  }


  /*
   *  clean up
   */

  /* update reference source for next ADC run */
  Cfg.Ref = ADC_REF_BANDGAP;       /* we've used the bandgap reference */

  DischargeProbes();               /* discharge DUT */

  return ESR;
}

#endif



#ifdef SW_OLD_ESR

/*
 *  set up Timer0 as MCU cycle timer
 *
 *  requires:
 *  - number of MCU cycles
 *
 *  returns:
 *  - 1 on success
 *  - 0 on error
 */

uint8_t SetupDelayTimer(uint8_t Cycles)
{
  uint8_t           Flag = 0;      /* return value */

  /*
   *  adjust cycles for DelayTimer():
   *  - -4 for function call
   *  - -3 for starting timer
   *  - -2 for waiting loop
   *  - -4 for stopping timer
   *  - -4 for return
   *  - total: 17
   */

  #define CYCLE_OFFSET   17

  if (Cycles < CYCLE_OFFSET) return Flag;    /* check for required minimum */
  Cycles -= CYCLE_OFFSET;                    /* substract cycles for DelayTimer() */ 

  #undef CYCLE_OFFSET


  /*
   *  set up timer0:
   *  - CTC mode (count up to OCR0A) 
   *  - prescaler 1 to match MCU cycles
   */

  TCCR0B = 0;                      /* stop timer */
  TCCR0A = (1 << WGM01);           /* set CTC mode, disable output compare pins */
  OCR0A = Cycles;                  /* set number of MCU cycles */
  /* todo: check if we have to substract one cycle for setting the flag */

  Flag = 1;              /* signal success */
  return Flag;
}



/*
 *  start timer and wait for timeout
 *  (requires prior call of SetupDelayTimer() for setup)
 */

void DelayTimer(void)
{
  TCNT0 = 0;                       /* reset counter to 0 */
  TCCR0B = (1 << CS00);            /* start timer by setting prescaler */

  while (!(TIFR0 & (1 << OCF0A)));   /* wait for output compare A match flag */

  TCCR0B = 0;                      /* stop timer */
  TIFR0 = (1 << OCF0A);            /* clear flag */
}



/*
 *  measure ESR
 *  - tolerates charge up to about 130mV
 *
 *  requires:
 *  - pointer to cap data structure
 *
 *  returns:
 *  - ESR in 0.01 Ohm
 *  - UINT16_MAX on any problem
 */

uint16_t MeasureESR(Capacitor_Type *Cap)
{
  uint16_t          ESR = UINT16_MAX;   /* return value */
  uint16_t          U_1;           /* voltage at probe 1 with pos. pulse unloaded */
  uint16_t          U_2;           /* voltage at probe 2 with pos. pulse loaded */
  uint16_t          U_3;           /* voltage at probe 2 with neg. pulse unloaded */
  uint16_t          U_4;           /* voltage at probe 1 with neg. pulse loaded */
  uint8_t           Probe1;        /* probe #1 */
  uint8_t           Probe2;        /* probe #2 */
  uint8_t           Bits;          /* register bits for ADC */
  uint8_t           n;             /* counter */
  uint32_t          Sum_1;         /* sum #1 */
  uint32_t          Sum_2;         /* sum #2 */
  uint32_t          Value;

  /* check for a capacitor >= 0.18�F */
  if ((Cap == NULL) ||
      (CmpValue(Cap->Value, Cap->Scale, 180, -9) < 0)) return ESR;


  /*
   *  init stuff
   */

  DischargeProbes();                    /* try to discharge probes */
  if (Check.Found == COMP_ERROR) return ESR;   /* skip on error */

  DischargeCap(Cap->A, Cap->B);         /* additional discharge */

  UpdateProbes2(Cap->A, Cap->B);        /* update probes */
  Probe1 = Probes.Ch_1;                 /* ADC MUX for probe-1 */
  Probe2 = Probes.Ch_2;                 /* ADC MUX for probe-2 */

  Probe1 |= ADC_REF_BANDGAP;            /* select bandgap reference */
  Probe2 |= ADC_REF_BANDGAP;            /* select bandgap reference */

  /* register bits to enable and start ADC */
  Bits = (1 << ADSC) | (1 << ADEN) | (1 << ADIF) | ADC_CLOCK_DIV;

  /* init variables */
  Sum_1 = 1;             /* 1 to prevent division by zero */
  Sum_2 = 1;             /* 1 to prevent division by zero */


  /*
   *  We have to create a delay to shift the middle of the pulse to the ADC's
   *  S&H. S&H happens at 1.5 ADC clock cycles after starting the conversion.
   *  We synchronize to a dummy conversion done directly before, so we have
   *  2.5 ADC clock cycles to S&H. The time between the completed dummy
   *  conversion and S&H of the next conversion is:
   *    2.5 ADC clock cycles
   *    - MCU cycles for waiting loop for completion of dummy conversion (4)
   *    - MCU cycles for starting next conversion (2)
   *    - 5�s delay
   *    - MCU cycles for enabling pulse (4)
   *
   *  That time is the first half of the puls. So we have to double the time
   *  for a full pulse. Half pulse for 8MHz MCU clock is about 13.5�s.
   */

  /* delay for pulse */
  /* MCU cycles for one ADC cycle * 2.5 - MCU cycles for 5�s - 10 */
  U_1 = ((MCU_CYCLES_PER_ADC * 25) / 10) - (MCU_CYCLES_PER_US * 5) - 10;
  n = (uint8_t)U_1;

  /* set up delay timer */
  if (SetupDelayTimer(n) == 0) return ESR;   /* skip on error */


  /*
   *  charge capacitor with a negative pulse of half length
   *  pulse: GND -- probe-2 / probe-1 -- Rl -- Vcc
   */

  ADC_PORT = 0;               /* set ADC port to low */
  ADMUX = Probe1;             /* set input channel to probe-1 & set bandgap ref */
  wait10ms();                 /* time for voltage stabilization */
  ADC_DDR = Probes.Pin_2;     /* pull down probe-2 directly */
  R_PORT = Probes.Rl_1;       /* pull up probe-1 via Rl */
  R_DDR = Probes.Rl_1;        /* enable resistor */
  DelayTimer();               /* wait 1/2 pulse */
  R_PORT = 0;                 /* set resistor port to low */
  R_DDR = 0;                  /* set resistor port to HiZ */


  /*
   *  measurement loop:
   *  - simulate AC by positive and negative pulses
   *  - measure start voltage (no load)
   *  - measure pulse voltage (with load)
   */  

  n = 255;
  while (n > 0)
  {
    /*
     *  forward mode, probe-1 only (probe-2 in HiZ mode)
     *  get voltage at probe-1 (facing Gnd)
     *  set probes: GND -- probe-1 -- Rl -- Vcc / probe-2 -- HiZ
     */

    ADC_DDR = Probes.Pin_1;        /* pull down probe-1 directly to GND */
    R_PORT = Probes.Rl_1;          /* pull up probe-1 via Rl */
    R_DDR = Probes.Rl_1;           /* enable resistor */
    ADMUX = Probe1;                /* set input channel to probe-1 & set bandgap ref */
    wdt_reset();                   /* reset watchdog */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    /* real conversion */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_1 = ADCW;                    /* save ADC value */


    /*
     *  forward mode, positive charging pulse
     *  get voltage at probe-2 (facing Vcc)
     *  set probes: GND -- probe-1 / probe-2 -- Rl -- Vcc
     */

    ADMUX = Probe2;                /* set input channel to probe-2 & set bandgap ref */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */

    /* read ADC in the mid of a positive charging pulse */
    ADCSRA = Bits;                 /* start conversion with next ADC clock cycle */
    wait5us();
    R_PORT = Probes.Rl_2;          /* pull up probe-2 via Rl */
    R_DDR = Probes.Rl_2;           /* enable resistor */
    DelayTimer();                  /* wait 1/2 pulse */
    DelayTimer();                  /* wait another 1/2 pulse */
    R_PORT = 0;                    /* set resistor port to low */
    R_DDR = 0;                     /* set resistor port to HiZ */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_2 = ADCW;                    /* save ADC value */


    /*
     *  prevent runaway of cap's charge
     */

    if (U_2 <= 100)
    {
      /* charge cap a little bit more (positive pulse) */

      /* set probes: GND -- probe-1 / probe-2 -- Rl -- Vcc */
      /* probe-1 is still pulled down directly */
      R_PORT = Probes.Rl_2;        /* pull up probe-2 via Rl */
      R_DDR = Probes.Rl_2;         /* enable pull up */
      wait2us();
      DelayTimer();                /* wait 1/2 pulse */
      R_DDR = 0;                   /* disable any pull up */      
      R_PORT = 0;                  /* reset probe resistors */
    }


    /*
     *  reverse mode, probe-2 only (probe-1 in HiZ mode)
     *  get voltage at probe 2 (facing Gnd)
     *  set probes: GND -- probe-2 -- Rl -- Vcc / probe-1 -- HiZ
     */

    ADC_DDR = Probes.Pin_2;        /* pull down probe-2 directly */
    R_PORT = Probes.Rl_2;          /* pull up probe-2 via Rl */
    R_DDR = Probes.Rl_2;           /* enable resistor */
    ADMUX = Probe2;                /* set input channel to probe-2 & set bandgap ref */
    wdt_reset();                   /* reset watchdog */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    /* real conversion */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_3 = ADCW;                    /* save ADC value */


    /*
     *  reverse mode, negative charging pulse
     *  get voltage at probe-1 (facing Vcc)
     *  set probes: GND -- probe-2 / probe-1 -- Rl -- Vcc
     */

    ADMUX = Probe1;                /* set input channel to probe-1 & set bandgap ref */
    /* run dummy conversion for ADMUX change */
    ADCSRA = Bits;                 /* start conversion */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */

    /* read ADC in the mid of a negatve charging pulse */
    ADCSRA = Bits;                 /* start conversion with next ADC clock cycle */
    wait5us();
    R_PORT = Probes.Rl_1;          /* pull up probe-1 via Rl */
    R_DDR = Probes.Rl_1;           /* enable resistor */
    DelayTimer();                  /* wait 1/2 pulse */
    DelayTimer();                  /* wait another 1/2 pulse */
    R_PORT = 0;                    /* set resistor port to low */
    R_DDR = 0;                     /* set resistor port to HiZ */
    while (ADCSRA & (1 << ADSC));  /* wait until conversion is done */
    U_4 = ADCW;                    /* save ADC value */


    /*
     *  prevent runaway of cap's charge
     */

    if (U_4 <= 100)           /* <= 107mV */
    {
      /* charge cap a little bit more (negative pulse) */

      /* set probes: GND -- probe-2 / probe-1 -- Rl -- Vcc */
      /* probe-2 is still pulled down directly */
      R_PORT = Probes.Rl_1;        /* pull up probe-1 via Rl */
      R_DDR = Probes.Rl_1;         /* enable pull up */
      wait2us();
      DelayTimer();                /* wait 1/2 pulse */
      R_DDR = 0;                   /* disable any pull up */      
      R_PORT = 0;                  /* reset probe resistors */
    }


    /*
     *  manage measured values
     */

    U_1 += U_3;          /* sum of both measurements without pulses/load */
    Sum_1 += U_1;        /* add to total no-load sum */
    U_2 += U_4;          /* sum of both measurements with pulses/load */
    Sum_2 += U_2;        /* add to total with-load sum */

    n--;                 /* next loop run */
  }


  /*
   *  process measurements
   */

  /* calculate voltage across the DUT */
  if (Sum_2 > Sum_1)               /* valid measurement */
  {
    Sum_2 -= Sum_1;                /* subtract voltage at DUT's low side (RiL) */
  }
  else                             /* invalid measurement */
  {
    Sum_2 = 0;
  }


  /*
   *  calculate ESR
   *  - ESR = U_ESR / I_ESR
   *    with U_ESR = (U2 or U4) and I_ESR = (U1 or U3) / RiL
   *    ESR = (U2 or U4) * RiL / (U1 or U3)
   *  - since we divide (U2 or U4) by (U1 or U3), we don't need to convert
   *    the ADC value into a voltage and simply desample the sums.
   *  - so ESR = Sum_2 * RiL / Sum_1
   *  - for a resolution of 0.01 Ohms we have to scale RiL to 0.01 Ohms
   */

  Value = (uint32_t)(NV.RiL * 10);      /* RiL in 0.01 Ohms */
  Value *= Sum_2;                       /* sum of raw values for voltage across DUT */
  Value /= Sum_1;                       /* sum of raw values for voltage at RiL */
  U_1 = (uint16_t)Value;                /* raw ESR (0.01 Ohms) */

  /* consider probe resistance */
  #ifdef R_MULTIOFFSET
    /* get index number for probe pair */
    n = GetOffsetIndex(Probes.ID_1, Probes.ID_2);
    U_2 = NV.RZero[n];
  #else
    U_2 = NV.RZero;
  #endif

  if (U_1 > U_2)              /* larger than offset */
  {
    U_1 -= U_2;               /* subtract offset */
    ESR = U_1;                /* we got a valid result */
  }


  /*
   *  clean up
   */

  /* update reference source for next ADC run */
  Cfg.Ref = ADC_REF_BANDGAP;       /* we've used the bandgap reference */

  DischargeProbes();               /* discharge DUT */

  return ESR;
}

#endif



/* ************************************************************************
 *   capacitance measurements
 * ************************************************************************ */


/*

We measure the capacitance by measuring the time needed to charge up the DUT
to a specific voltage using a constant voltage source:

  U_c(t) = U_in * (1 - e^(-t/RC))

With ln(e^x) = x we get

  C = -t / (R * ln(1 - U_c/U_in) 

for an ideal capacitor whithout parallel resistive losses by leakage.

Instead of calculating C directly we'll use pre-calculated tables to speed
up things and keep the firmware small. The tables hold the pre-calculated
values of -1/(R * ln(1 - U_c/U_in) for a specific range of U_c, so we just
have to multiply the time with that stored factor to get C.

Large caps:
- R = 680 + 22 (22 is the internal resistance of the MCU for pull-up)
- U_in = 5V
- values are: (-1 / (R * ln(1 - U_c/U_in))) * 10^9n * 10^-2s * 10^-1
  - 10^9n for nF scale
  - 10^-2s for charge pulses of 10ms each
  - 10^-1 internal scale factor (make values fit in uint16_t)
- bc:
  - options: -i -l
  - define x (u) { return (-1000000 / (702 * l(1 - u/5000))); }

Small caps:
- R = 470k (neglect internal resistance of uC)
- U_in = 5V
- values are: (-1 / (R * ln(1 - U_c/U_in))) * 10^12p * 10^-4
  - 10^12p for pF scale
  - 10^-4 internal scale factor (make values fit in uint16_t)
- bc:
  - options: -i -l
  - define x (u) { return (-100000000 / (470000 * l(1 - u/5000))); }
- We could use 10^-3 as internal scale factor to maximize resolution.

*/


/*
 *  measure cap >4.7�F between two probe pins
 *
 *  requires:
 *  - Cap: pointer to capacitor data structure 
 *
 *  returns:
 *  - 3 on success
 *  - 2 if capacitance is too low
 *  - 1 if capacitance is too high
 *  - 0 on any problem
 */

uint8_t LargeCap(Capacitor_Type *Cap)
{
  uint8_t           Flag = 3;      /* return value */
  uint8_t           TempByte;      /* temp. value */
  uint8_t           Mode;          /* measurement mode */
  int8_t            Scale;         /* capacitance scale */
  uint16_t          TempInt;       /* temp. value */
  uint16_t          Pulses;        /* number of charging pulses */
  int16_t           U_Zero;        /* voltage before charging (zero offset) */
  uint16_t          U_Cap;         /* voltage of DUT */
  uint16_t          U_Drop = 0;    /* voltage drop (self-discharge) */
  uint16_t          U_leak = 0;    /* voltage drop (leakage current) */
  uint32_t          Raw;           /* raw capacitance value */
  uint32_t          Value;         /* corrected capacitance value */

  /* set up mode */
  Mode = PULL_10MS | PULL_UP;      /* start with large cap (>47uF) */


  /*
   *  We charge the DUT with up to 500 pulses each 10ms long until the
   *  DUT reaches 300mV. The charging is done via Rl. This method is
   *  suitable for large capacitances from 47uF up to 100mF. If we find a 
   *  lower capacitance we'll switch to 1ms charging pulses and try again
   *  (4.7�F up to 47�F).
   *
   *  Problem:
   *  ReadADC() needs about 5ms (44 runs). We charge the DUT for 10ms and
   *  measure for 5ms. During that time the voltage will drop due to
   *  resistive losses of the DUT and the measurement itself. So the DUT
   *  seems to need more time to reach 300mV causing a higher capacitance
   *  be calculated.
   *
   *  Remark:
   *  The Analog Input Resistance of the ADC is 100MOhm typically.
   */

large_cap:

  /* prepare probes */
  DischargeProbes();                    /* try to discharge probes */
  if (Check.Found == COMP_ERROR) return 0;     /* skip on error */


  /*
   *  get zero offset (noise / dielectric absorption)
   *  - create reference point with a low positive voltage to be able to
   *    measure also a low negative offset
   *  - use voltage divider: top RiH + Rl, bottom RiL (about 140mV)
   */

  /* set probes: Gnd -- probe-2 -- Rl - Vcc / probe-1 -- HiZ */
  ADC_PORT = 0;                    /* set ADC port to low */
  ADC_DDR = Probes.Pin_2;          /* pull down probe-2 directly */
  R_PORT = Probes.Rl_2;            /* pull up probe-2 via Rl */
  R_DDR = Probes.Rl_2;             /* enable pull-up */
  U_Zero = ReadU(Probes.Ch_1);     /* get voltage at probe-1 */
  U_Zero -= ReadU(Probes.Ch_2);    /* - voltage at probe-2 */

  /* set probes: Gnd -- probe-2 / probe-1 -- HiZ */
  R_PORT = 0;                      /* set resistor port to low */
  R_DDR = 0;                       /* set resistor port to HiZ */  


  /* charge DUT with up to 500 pulses until it reaches 300mV */
  /* pulse: probe-1 -- Rl -- Vcc */
  Pulses = 0;                      /* reset number of pulses */
  TempByte = 1;                    /* set loop control */
  while (TempByte)                 /* charge loop */
  {
    Pulses++;
    PullProbe(Probes.Rl_1, Mode);       /* charging pulse */
    U_Cap = ReadU(Probes.Ch_1);         /* get voltage */

    /* consider zero offset */
    if (U_Cap > U_Zero)            /* voltage higher than zero offset */
      U_Cap -= U_Zero;                  /* subtract zero offset */
    else                           /* shouldn't happen but you never know */
      U_Cap = 0;                        /* assume 0V */

    /* end loop if charging is too slow */
    if ((Pulses == 126) && (U_Cap < 75)) TempByte = 0;
    
    /* end loop if 300mV are reached */
    if (U_Cap >= 300) TempByte = 0;

    /* end loop if maximum number of pulses is reached (timeout) */
    if (Pulses == 500) TempByte = 0;

    wdt_reset();                        /* reset watchdog */
  }

  /* if 300mV are not reached DUT isn't a cap or much too large (>100mF) */
  /* we can ignore that for mid-sized caps */
  if (U_Cap < 300)
  {
    Flag = 1;                           /* signal too high capacitance */
  }

  /* if 1300mV are reached with one pulse, we got a small cap */
  if ((Pulses == 1) && (U_Cap > 1300))
  {
    if (Mode & PULL_10MS)               /* 10ms pulses (>47�F) */
    {
      /* change to smaller cap (4.7 - 47�F) */
      Mode = PULL_1MS | PULL_UP;        /* set mode to 1ms charging pulses */
      goto large_cap;                   /* and re-run */
    }
    else                                /* 1ms pulses (<47�F) */
    {
      Flag = 2;                         /* signal low capacitance (<4.7�F) */
    }
  }


  /*
   *  Check if DUT sustains the charge and get the voltage drop.
   *  - Run for about the same time as before (minus the 1 or 10ms charging time).
   *  - Ignore the MCU cycles for the conditions in the charge loop (about 20)
   *    as they are just a few in comparison to the ADC conversion.
   *  - Also run ADC conversions to include charge losses by ADC.
   *  - This gives us an approximation of the leakage.
   */

  if (Flag == 3)              /* no issues so far */
  {
    /* check self-discharging for measuring period */
    U_Drop = ReadU(Probes.Ch_1);        /* get start voltage */
    TempInt = Pulses;                   /* same number of loop runs (pulses) */
    while (TempInt > 0)                 /* delay loop */
    {
      TempInt--;                        /* decrease timeout */
      U_leak = ReadU(Probes.Ch_1);      /* get current voltage */

      wdt_reset();                      /* reset watchdog */
    }

    /* calculate voltage drop */
    if (U_Drop > U_leak)           /* sanity check */
    {
      U_Drop -= U_leak;            /* voltage drop */

      #ifdef SW_C_VLOSS
      /* voltage loss in 0.1% */
      Cap->U_loss = (uint16_t)((unsigned long)(U_Drop * 1000UL) / U_Cap);
      #endif
    }
    else                           /* bad values */
    {
      U_Drop = 0;                  /* zero drop */
    }

    /* if voltage drop is too large, consider DUT not to be a cap */
    if (U_Drop > 100) Flag = 0;


    /*
     *  Take a second measurement with a specific delay to 
     *  determine the self-discharge leakage current.
     *  - The charge required for the ADC conversion can be neglected because
     *    C_S/H is just 14pF (very small vs. DUT).
     */

    U_leak = ReadU(Probes.Ch_1);        /* get start voltage */

    if (Mode & PULL_10MS)     /* > 47�F */
    {
      wait1000ms();
    }
    else                      /* < 47�F */
    {
      wait100ms();
    }

    TempInt = ReadU(Probes.Ch_1);       /* get voltage after delay */

    /* calculate voltage drop */
    if (U_leak > TempInt)               /* sanity check */
    {
      #if 0
      /* SW_C_VLOSS alternative */
      uint16_t           U_Temp;        /* temp. value */

      U_Temp = U_leak;                  /* save voltage */
      #endif

      U_leak -= TempInt;                /* voltage drop */

      #if 0
      /* SW_C_VLOSS alternative */
      /* voltage loss in 0.1% */
      Cap->U_loss = (unsigned long)(U_leak * 1000UL) / U_Temp;
      #endif
    }
    else                                /* no drop */
    {
      U_leak = 0;                       /* drop is zero */
    }
  }


  /*
   *  calculate capacitance
   *  - use factor from pre-calculated LargeCap_table
   *  - ignore NV.CapZero since it's in the pF range
   *  - consider voltage drop by ADC and leakage
   */

  if (Flag == 3)              /* no issues so far */
  {
    Scale = -9;                           /* factor is scaled to nF */
    /* get interpolated factor from table */
    Raw = GetFactor(U_Cap + U_Drop, TABLE_LARGE_CAP);
    Raw *= Pulses;                        /* C = pulses * factor */
    if (Mode & PULL_10MS) Raw *= 10;      /* *10 for 10ms charging pulses */

    if (Raw > (UINT32_MAX / 1000))        /* scale down if C >4.3mF */
    {
      Raw /= 1000;                        /* scale down by 10^3 */
      Scale += 3;                         /* add 3 to the exponent */
    }

    Value = Raw;                          /* copy raw value */

    /*
     *  We got a systematic error which needs to be compensated.
     *  The compensation factor can vary with the tester model.
     */

    Value *= 1000;                        /* scale for 0.1% resolution */
    if (Mode & PULL_10MS)          /* cap >47�F */
    {
      Value /= (1000 - CAP_FACTOR_LARGE);    /* apply factor (in 0.1%) */
    }
    else                           /* cap 4.7-47�F */
    {
      Value /= (1000 - CAP_FACTOR_MID);      /* apply factor (in 0.1%) */
    }

    /* copy data */
    Cap->A = Probes.ID_2;     /* pull-down probe pin */
    Cap->B = Probes.ID_1;     /* pull-up probe pin */
    Cap->Scale = Scale;       /* -9 (nF) or -6 (�F) */
    Cap->Raw = Raw;           /* uncompensated value */
    Cap->Value = Value;       /* compensated value */
                              /* max. 4.3*10^6nF or 100*10^3�F */


    /*
     *  Calculate the self-discharge leakage current
     *  - with Q = C * U and I = Q / t
     *    we get I = C * U_diff / t
     */
    
    while (Value > 800000)    /* rescale if necessary */
    {
      Value /= 10;
      Scale++;
    }

    /* I = C * U_diff / t */
    Value *= U_leak;          /* * U_diff (mV) */
    Value /= 1000;            /* scale to V */
    if (Mode & PULL_1MS)      /* short pulses */
    {
      Scale++;                /* / 0.1s */    
    }
    /* long pulses: / 1s */

    while (Value > UINT16_MAX)     /* scale to uint16_t */
    {
      Value /= 10;
      Scale++;
    }

    Cap->I_leak_Value = (uint16_t)Value;     /* save result */
    Cap->I_leak_Scale = Scale;
  }

  return Flag;
}



/*
 *  measure cap <4.7�F between two probe pins
 *
 *  requires:
 *  - Cap: pointer to capacitor data structure 
 *
 *  returns:
 *  - 3 on success
 *  - 2 if capacitance is too low
 *  - 1 if capacitance is too high
 *  - 0 on any problem
 */

uint8_t SmallCap(Capacitor_Type *Cap)
{
  uint8_t           Flag = 3;      /* return value */
  uint8_t           TempByte;      /* temp. value */
  int8_t            Scale;         /* capacitance scale */
  uint16_t          Ticks;         /* timer counter */
  uint16_t          Ticks2;        /* timer overflow counter */
  #ifndef HW_ADJUST_CAP
  uint16_t          U_c;           /* voltage of capacitor */
  #endif
  uint32_t          Raw;           /* raw capacitance value */
  uint32_t          Value;         /* corrected capacitance value */


  /*
   *  Measurement method used for small caps < 4.7uF (was 50�F):
   *  We need a much better resolution for the time measurement. Therefore we
   *  use the MCUs internal 16-bit counter and analog comparator. The counter
   *  inceases until the comparator detects that the voltage of the DUT is as
   *  high as the internal bandgap reference. To support the higher time
   *  resolution we use the Rh probe resistor for charging.
   *
   *  Remark:
   *  The analog comparator has an Input Leakage Current of -50nA up to 50nA 
   *  at Vcc/2. The Input Offset is <10mV at Vcc/2.
   */

  Ticks2 = 0;                           /* reset timer overflow counter */

  /*
   *  init hardware
   */

  /* prepare probes */
  DischargeProbes();                    /* try to discharge probes */
  if (Check.Found == COMP_ERROR) return 0;     /* skip on error */

  /* set probes: Gnd -- all probes / Gnd -- Rh -- probe-1 */
  R_PORT = 0;                           /* set resistor port to low */
  /* set ADC probe pins to output mode */
  ADC_DDR = (1 << TP1) | (1 << TP2) | (1 << TP3);
  ADC_PORT = 0;                         /* set ADC port to low */
  R_DDR = Probes.Rh_1;                  /* pull-down probe-1 via Rh */

  /* set up analog comparator */
  ADCSRB = (1 << ACME);                 /* use ADC multiplexer as negative input */
  ACSR = (1 << ACBG) | (1 << ACIC);     /* use bandgap as positive input, trigger Timer1 */
  ADMUX = ADC_REF_VCC | Probes.Ch_1;    /* switch ADC multiplexer to probe 1 */
                                        /* and set AREF to Vcc */
  ADCSRA = ADC_CLOCK_DIV;               /* disable ADC, but keep clock dividers */
  wait200us();

  /* set up timer */
  TCCR1A = 0;                           /* set default mode */
  TCCR1B = 0;                           /* set more timer modes */
  /* timer stopped, falling edge detection, noise canceler disabled */
  TCNT1 = 0;                            /* set Counter1 to 0 */
  /* clear all flags (input capture, compare A & B, overflow */
  TIFR1 = (1 << ICF1) | (1 << OCF1B) | (1 << OCF1A) | (1 << TOV1);
  R_PORT = Probes.Rh_1;                 /* pull-up probe-1 via Rh */
                                        
  /* enable timer */
  if (Check.Found == COMP_FET)     /* measuring C_GS */  
  {
    /* keep all probe pins pulled down but probe-1 */
    TempByte = ((1 << TP1) | (1 << TP2) | (1 << TP3)) & ~(Probes.Pin_1);
  }
  else                             /* normal measurement */
  {
    TempByte = Probes.Pin_2;            /* keep just probe-2 pulled down */
  }

  /* start timer by setting clock prescaler (1/1 clock divider) */
  TCCR1B = (1 << CS10);
  ADC_DDR = TempByte;                   /* start charging DUT */


  /*
   *  timer loop
   *  - run until voltage is reached
   *  - detect timer overflow
   */

   while (1)
   {
     TempByte = TIFR1;                  /* get Timer1 flags */

     /* end loop if input capture flag is set (= same voltage) */
     if (TempByte & (1 << ICF1)) break;

     /* detect timer overflow by checking the overflow flag */
     if (TempByte & (1 << TOV1))
     {
       /* happens at 65.536ms for 1MHz or 8.192ms for 8MHz */
       TIFR1 = (1 << TOV1);             /* reset flag */
       wdt_reset();                     /* reset watchdog */
       Ticks2++;                        /* increase overflow counter */

       /* end loop if charging takes too long (13.1s) */
       if (Ticks2 == (CPU_FREQ / 5000)) break;
     }
   }

  /* stop counter */
  TCCR1B = 0;                           /* stop timer */
  TIFR1 = (1 << ICF1);                  /* reset Input Capture flag */

  Ticks = ICR1;                         /* get counter value */

  /* disable charging */
  R_DDR = 0;                  /* set resistor port to HiZ mode */

  /* catch missed timer overflow */
  if ((TCNT1 > Ticks) && (TempByte & (1 << TOV1)))
  {
    TIFR1 = (1 << TOV1);                /* reset overflow flag */
    Ticks2++;                           /* increase overflow counter */
  }

  /* enable ADC again */
  ADCSRA = (1 << ADEN) | (1 << ADIF) | ADC_CLOCK_DIV;
  ADCSRB &= ~(1 << ACME);     /* disable ADC multiplexer as negative input */

  #ifndef HW_ADJUST_CAP
  /* get voltage of DUT */
  U_c = ReadU(Probes.Ch_1);        /* get voltage of cap */
  #endif

  /* start discharging DUT */
  R_PORT = 0;                      /* pull down probe-1 via Rh */
  R_DDR = Probes.Rh_1;             /* enable Rh for probe-1 again */

  /* skip measurement if charging took too long */
  if (Ticks2 >= (CPU_FREQ / 5000)) Flag = 1;


  /*
   *  calculate capacitance
   *  - use factor from pre-calculated SmallCap_table
   */

  if (Flag == 3)              /* measurement successful */
  {
    /*  combine both counter values */
    Raw = (uint32_t)Ticks;                /* set lower 16 bits */
    Raw |= (uint32_t)Ticks2 << 16;        /* set upper 16 bits */
    if (Raw > 2) Raw -= 2;                /* subtract processing time overhead */

    Scale = -12;                          /* default factor is for pF scale */
    if (Raw > (UINT32_MAX / 1000))        /* prevent overflow (4.3*10^6) */
    {
      Raw /= 1000;                        /* scale down by 10^3 */
      Scale += 3;                         /* add 3 to the exponent (nF) */
    }

    /* multiply with factor from table */
    Raw *= GetFactor(Cfg.Bandgap + NV.CompOffset, TABLE_SMALL_CAP);

    /* divide by CPU frequency to get the time and multiply with table scale */
    Raw /= (CPU_FREQ / 10000);

    #if CAP_FACTOR_SMALL != 0
    /*
     *  Optional compensation
     *  - we have to apply the compensation to the raw value because of
     *    the zero-offset (keeping the relation matched)
     */

    Raw *= 1000;                          /* scale for 0.1% resolution */
    Raw /= (1000 - CAP_FACTOR_SMALL);     /* apply factor (in 0.1%) */
    #endif

    Value = Raw;                          /* take raw value */

    /* take care about zero offset if feasable */
    if (Scale == -12)                     /* pF scale */
    {
      #ifdef CAP_MULTIOFFSET
      /* get index number for probe pair */
      TempByte = GetOffsetIndex(Probes.ID_1, Probes.ID_2);

      if (Value >= NV.CapZero[TempByte])  /* if value is larger than offset */
      {
        Value -= NV.CapZero[TempByte];    /* substract offset */
      }
      #else
      if (Value >= NV.CapZero)            /* if value is larger than offset */
      {
        Value -= NV.CapZero;              /* substract offset */
      }
      #endif
      else                                /* if value is smaller than offset */
      {
        /* we have to prevent a negative value */
        Value = 0;                        /* set value to 0 */
      }
    }

    /* copy data */
    Cap->A = Probes.ID_2;     /* pull-down probe pin */
    Cap->B = Probes.ID_1;     /* pull-up probe pin */
    Cap->Scale = Scale;       /* -12 or -9 */
    Cap->Raw = Raw;
    Cap->Value = Value;       /* max. 5.1*10^6pF or 125*10^3nF */


    #ifndef HW_ADJUST_CAP
    /*
     *  Self-adjust the voltage offset of the analog comparator and internal
     *  bandgap reference if C is 100nF up to 20�F. The minimum of 100nF
     *  should keep the voltage stable long enough for the measurements. 
     *  Changed offsets will be used in the next test run.
     */

    if (((Scale == -12) && (Value >= 100000)) ||
        ((Scale == -9) && (Value <= 20000)))
    {
      int16_t         Offset;
      int32_t         TempLong;

      /*
       *  We can self-adjust the offset of the internal bandgap reference
       *  by measuring a voltage lower than the bandgap reference, one time
       *  with the bandgap as reference and a second time with Vcc as
       *  reference. The common voltage source is the cap we just measured.
       */

       while (ReadU(Probes.Ch_1) > 980)      /* discharge below bandgap ref */ 
       {
         /* keep discharging */
       }

       R_DDR = 0;                       /* stop discharging */

       Cfg.AutoScale = 0;               /* disable auto scaling */
       Ticks = ReadU(Probes.Ch_1);      /* U_c with Vcc reference */
       Cfg.AutoScale = 1;               /* enable auto scaling again */
       Ticks2 = ReadU(Probes.Ch_1);     /* U_c with bandgap reference */

       R_DDR = Probes.Rh_1;             /* resume discharging */

       Offset = Ticks - Ticks2;         /* difference */
       Ticks = Cfg.Bandgap;             /* current U_bandgap incl. offset */

       /* allow some difference caused by the different voltage resolutions
          (4.88 vs. 1.07) */
       if ((Offset < -4) || (Offset > 4))    /* difference too large */
       {
         /*
          *  Calculate offset:
          *  - first get offset per mV:
          *    factor = Delta / U_c_bandgap
          *  - offset for U_ref: 
          *    Offset = factor * U_ref = (Delta / U_c_bandgap) * U_ref
          *           = (Delta * U_ref) / U_c_bandgap
          */

         TempLong = Offset;                  /* delta */
         TempLong *= Ticks;                  /* * U_ref */
         TempLong /= Ticks2;                 /* / U_c_bandgap */

         NV.RefOffset += (int8_t)TempLong;   /* update offset */
         Ticks += (int8_t)Value;             /* update local U_bandgap */
       }


      /*
       *  In the cap measurement using the analog comparator we compared
       *  the voltage of the cap to the bandgap reference. Since the MCU
       *  has an internal voltage offset for the analog comparator, the
       *  MCU used actually U_bandgap + U_offset. We get that offset by
       *  comparing the bandgap reference with the voltage of the cap:
       *  U_c = U_bandgap + U_offset -> U_offset = U_c - U_bandgap
       */

      Offset = U_c - Ticks;

      /* limit offset to a valid range of -50mV - 50mV */
      if ((Offset > -50) && (Offset < 50))
      {
        NV.CompOffset = Offset;         /* update offset */
      }
    }
    #endif

    #ifdef SW_C_VLOSS
    uint16_t             U_Zero;        /* zero offset */

    /*
     *  get V_loss (in 0.1%)
     *  - discharge cap and measure voltage offset
     *  - charge cap for a specific time and measure start voltage
     *  - wait for a specific time and measure end voltage
     *  - calculate V_loss
     *  - based on Karl-Heinz' GetVloss()
     */

    if (CmpValue(Value, Scale, 50, -9) == 1)      /* > 50nF */
    {
      /* use value in 10nF for timing */
      Ticks = RescaleValue(Value, Scale, -8);     /* rescale to 10nF */

      /* discharge cap */
      DischargeProbes();                          /* try to discharge probes */
      if (Check.Found == COMP_ERROR) return 0;    /* skip on error */

      /* get zero offset */
      /* set probes: Gnd -- probe-2 / Gnd -- Rl -- probe-1 */
      ADC_PORT = 0;                     /* set ADC port to low */      
      ADC_DDR = Probes.Pin_2;           /* pull down probe #2 directly */
      R_PORT = 0;                       /* set R port to low */
      R_DDR = Probes.Rl_1;              /* pull down probe #1 via Rl */
      U_Zero = ReadU(Probes.Ch_1);      /* voltage at probe #1 */

      /* charge cap for a specific time (half of time units) */
      /* set probes: Gnd -- probe-2 / probe-1 -- Rl - Vcc */
      R_PORT = Probes.Rl_1;             /* pull up probe #1 via Rl */
      Ticks2 = Ticks / 2;               /* 50% of time units */
      while (Ticks2)                    /* delay loop */
      {
        wait5us();                      /* wait 5�s */
        Ticks2--;                       /* next time unit */
      }

      /* get start voltage */
      /* set probes: Gnd -- probe-2 / probe-1 -- HiZ */
      R_DDR = 0;                        /* set R port to HiZ mode */
      R_PORT = 0;                       /* set R port to low */
      wdt_reset();                      /* reset watchdog */
      Cfg.Samples = 5;                  /* just 5 samples to reduce loss of charge */
      Ticks2 = ReadU(Probes.Ch_1);      /* voltage at probe #1 */
      if (Ticks2 > U_Zero)              /* sanity check */
      {
        Ticks2 -= U_Zero;               /* consider zero offset */
      }
      else                              /* no voltage rise */
      {
        Ticks2 = 0;                     /* reset voltage */
      }

      /* wait for a specific time (full time) */
      while (Ticks)                     /* delay loop */
      {
        wait5us();                      /* wait 5�s */
        Ticks--;                        /* next time unit */
      }

      /* get end voltage */
      Ticks = ReadU(Probes.Ch_1);       /* voltage at probe #1 */
      Cfg.Samples = ADC_SAMPLES;        /* set ADC samples back to default */
      wdt_reset();                      /* reset watchdog */
      if (Ticks > U_Zero)               /* sanity check */
      {
        Ticks -= U_Zero;                /* consider zero offset */
      }
      else                              /* no voltage rise */
      {
        Ticks = 0;                      /* reset voltage */
      }

      /* calculate V_loss */
      if (Ticks2 > Ticks)               /* sanity check */
      {
        U_Zero = Ticks2 - Ticks;        /* voltage drop */
        /* voltage loss in 0.1% */
        Cap->U_loss = (uint16_t)((unsigned long)(U_Zero * 500UL) / Ticks2);
      }
    }
    #endif
  }

  return Flag;
}



/*
 *  measure capacitance between two probe pins
 *
 *  requires:
 *  - Probe1: ID of probe to be pulled up [0-2]
 *  - Probe2: ID of probe to be pulled down [0-2]
 *  - ID: capacitor ID [0-2]
 */

void MeasureCap(uint8_t Probe1, uint8_t Probe2, uint8_t ID)
{
  uint8_t           TempByte;           /* temp. value */
  Capacitor_Type    *Cap;               /* pointer to cap data structure */
  Diode_Type        *Diode;             /* pointer to diode data structure */
  Resistor_Type     *Resistor;          /* pointer to resistor data structure */


  /*
   *  init
   */

  /* reset cap data */
  Cap = &Caps[ID];            /* get pointer */
  Cap->A = 0;
  Cap->B = 0;
  Cap->Scale = -12;           /* pF by default */
  Cap->Raw = 0;
  Cap->Value = 0;
  Cap->I_leak_Value = 0;
  #ifdef SW_C_VLOSS
  Cap->U_loss = 0;
  #endif

  if (Check.Found == COMP_ERROR) return;    /* skip check on any error */


  /*
   *  Normaly we would skip resistors, but a resistor < 10 Ohms could be
   *  also a large cap.
   */

  if (Check.Found == COMP_RESISTOR)
  {
    Resistor = &Resistors[0];         /* pointer to first resistor */
    TempByte = 0;

    while (TempByte < Check.Resistors)       /* loop through all resistors */
    {
      /* got matching pins */
      if (((Resistor->A == Probe1) && (Resistor->B == Probe2)) ||
          ((Resistor->A == Probe2) && (Resistor->B == Probe1)))
      {
        /* check for low value < 10 Ohms */
        if (CmpValue(Resistor->Value, Resistor->Scale, 10UL, 0) == -1)
          TempByte = 99;                /* signal low resistance and end loop */
      }

      TempByte++;      /* next one */
      Resistor++;      /* next one */  
    }

    /* we got a valid resistor */ 
    if (TempByte != 100) return;        /* skip this one */
  }


  /*
   *  Skip measurement for "dangerous" diodes
   *  - when Vf collides with the voltage of the capacitance measurement
   */

  Diode = SearchDiode(Probe1, Probe2);  /* search for matching diode */
  if (Diode != NULL)                    /* got it */
  {
    if (Diode->V_f < 1500)              /* Vf too low */
    {
      return;                           /* skip this one */
    }
  }


  /*
   *  run measurements
   */

  UpdateProbes2(Probe1, Probe2);        /* update register bits and probes */

  /* first run measurement for large caps */ 
  TempByte = LargeCap(Cap);

  /* if cap is too small run measurement for small caps */
  if (TempByte == 2)
  {
    TempByte = SmallCap(Cap);
  }


  /*
   *  check for plausibility
   *  - skip diodes which could be detected as capacitors
   *  - skip any transistor
   */

  if (Check.Found < COMP_DIODE)
  {
    /* low resistance might be a large cap */
    if (Check.Found == COMP_RESISTOR)
    {
      /* report capacitor only for a large C (> 4.3�F) */
      if (Cap->Scale >= -6) Check.Found = COMP_CAPACITOR;
    }

    /* we consider values below 5pF being just ghosts */
    else if ((Cap->Scale > -12) || (Cap->Value >= 5UL))
    {
      Check.Found = COMP_CAPACITOR;     /* report capacitor */
    }
  }


  /*
   *  clean up
   */

  DischargeProbes();               /* discharge DUT */

  /* reset all ports and pins */
  ADC_DDR = 0;                     /* set ADC port to input */
  ADC_PORT = 0;                    /* set ADC port low */
  R_DDR = 0;                       /* set resistor port to input */
  R_PORT = 0;                      /* set resistor port low */
}



#ifdef HW_ADJUST_CAP

/*
 *  use fixed reference cap to determine voltage offsets
 *  - cap: 100nF - 1000nF
 *  - based on method from SmallCap() for caps < 4.7�F
 *
 *  returns:
 *  - 1 on success
 *  - 0 on any problem
 */

uint8_t RefCap(void)
{
  uint8_t           Flag = 0;      /* return value */
  uint8_t           TempByte;      /* temp. value */
  uint16_t          Ticks;         /* timer counter */
  uint16_t          Ticks2;        /* timer overflow counter */
  uint16_t          U_c;           /* voltage of capacitor */
  int16_t           Offset;        /* voltage offset */
  int32_t           Value;         /* temp. value */


  /*
   *  fixed setup:
   *  Gnd -- cap -- ADC pin -- Rh -- resistor control pin
   */

  /* discharge cap */
  ADC_DDR &= ~(1 << TP_CAP);            /* set ADC pin to HiZ */
  ADJUST_DDR |= (1 << ADJUST_RH);       /* set Rh control pin to output */
  ADJUST_PORT &= ~(1 << ADJUST_RH);     /* pull down cap via Rh */
  TempByte = 0;
  while (TempByte <= 50)                /* discharge loop */
  {
    U_c = ReadU(TP_CAP);                /* get voltage of cap */
    if (U_c <= CAP_DISCHARGED)          /* seems to be discharged */
    {
      TempByte = 100;                   /* end loop */
    }
    else                                /* not discharged yet */
    {
      TempByte++;                       /* increase counter */
      MilliSleep(20);                   /* wait 20ms */
    }
  }

  if (TempByte != 100) return Flag;     /* timeout */


  /*
   *  setup hardware for measurement
   */

  Ticks2 = 0;                           /* reset timer overflow counter */

  /* set up analog comparator */
  ADCSRB = (1 << ACME);                 /* use ADC multiplexer as negative input */
  ACSR =  (1 << ACBG) | (1 << ACIC);    /* use bandgap as positive input, trigger Timer1 */
  ADMUX = ADC_REF_VCC | TP_CAP;         /* switch ADC multiplexer to cap pin */
                                        /* and set AREF to Vcc */
  ADCSRA = ADC_CLOCK_DIV;               /* disable ADC, but keep clock dividers */
  wait200us();

  /* set up timer */
  TCCR1A = 0;                           /* set default mode */
  TCCR1B = 0;                           /* set more timer modes */
  /* timer stopped, falling edge detection, noise canceler disabled */
  TCNT1 = 0;                            /* set Counter1 to 0 */
  /* clear all flags (input capture, compare A & B, overflow */
  TIFR1 = (1 << ICF1) | (1 << OCF1B) | (1 << OCF1A) | (1 << TOV1);
 
  /* start timer by setting clock prescaler (1/1 clock divider) */
  TCCR1B = (1 << CS10);
  ADJUST_PORT |= (1 << ADJUST_RH);      /* start charging DUT (pull up) */


  /*
   *  timer loop
   *  - run until voltage is reached
   *  - detect timer overflow
   */

   while (1)
   {
     TempByte = TIFR1;                  /* get Timer1 flags */

     /* end loop if input capture flag is set (= same voltage) */
     if (TempByte & (1 << ICF1)) break;

     /* detect timer overflow by checking the overflow flag */
     if (TempByte & (1 << TOV1))
     {
       /* happens at 65.536ms for 1MHz or 8.192ms for 8MHz */
       TIFR1 = (1 << TOV1);             /* reset flag */
       wdt_reset();                     /* reset watchdog */
       Ticks2++;                        /* increase overflow counter */

       /* end loop if charging takes too long (13.1s) */
       if (Ticks2 == (CPU_FREQ / 5000)) break;
     }
   }

  /* stop counter */
  TCCR1B = 0;                           /* stop timer */
  TIFR1 = (1 << ICF1);                  /* reset Input Capture flag */

  /* disable charging */
  ADJUST_DDR &= ~(1 << ADJUST_RH);      /* set Rh pin to HiZ mode */

  /* catch missed timer overflow */
  if (TIFR1 & (1 << TOV1))
  {
    TIFR1 = (1 << TOV1);                /* reset overflow flag */
  }

  /* enable ADC again */
  ADCSRA = (1 << ADEN) | (1 << ADIF) | ADC_CLOCK_DIV;
  ADCSRB &= ~(1 << ACME);     /* disable ADC multiplexer as negative input */

  /* get voltage of DUT */
  U_c = ReadU(TP_CAP);                  /* get voltage of cap */

  /* start discharging DUT */
  ADJUST_PORT &= ~(1 << ADJUST_RH);     /* pull down via Rh */
  ADJUST_DDR |= (1 << ADJUST_RH);       /* output mode */

  /* check for charging timeout */
  if (Ticks2 < (CPU_FREQ / 5000)) Flag = 1;   /* ok to proceed */


  /*
   *  get offsets
   */

  if (Flag)
  {
    /*
     *  We can self-adjust the offset of the internal bandgap reference
     *  by measuring a voltage lower than the bandgap reference, one time
     *  with the bandgap as reference and a second time with Vcc as
     *  reference. The common voltage source is the cap we just measured.
     */

    while (ReadU(TP_CAP) > 980)         /* discharge below bandgap ref */
    {
      /* keep discharging */
    }

    ADJUST_DDR &= ~(1 << ADJUST_RH);    /* stop discharging */

    Cfg.AutoScale = 0;                  /* disable auto scaling */
    Ticks = ReadU(TP_CAP);              /* U_c with Vcc reference */
    Cfg.AutoScale = 1;                  /* enable auto scaling again */
    Ticks2 = ReadU(TP_CAP);             /* U_c with bandgap reference */

    ADJUST_DDR |= (1 << ADJUST_RH);     /* resume discharging */

    Offset = Ticks - Ticks2;            /* difference */
    Ticks = Cfg.Bandgap;                /* current U_bandgap incl. offset */

    /* allow some difference caused by the different voltage resolutions
       (4.88 vs. 1.07) */
    if ((Offset < -4) || (Offset > 4))  /* difference too large */
    {
      /*
       *  Calculate offset:
       *  - first get offset per mV:
       *    factor = Delta / U_c_bandgap
       *  - offset for U_ref: 
       *    Offset = factor * U_ref = (Delta / U_c_bandgap) * U_ref
       *           = (Delta * U_ref) / U_c_bandgap
       */

      Value = Offset;              /* delta */
      Value *= Ticks;              /* * U_ref */
      Value /= Ticks2;             /* / U_c_bandgap */

      NV.RefOffset += (int8_t)Value;    /* update offset */
      Ticks += (int8_t)Value;           /* update local U_bandgap */
      Cfg.Bandgap = Ticks;              /* update global U_bandgap */
    }


    /*
     *  In the cap measurement using the analog comparator we compared
     *  the voltage of the cap to the bandgap reference. Since the MCU
     *  has an internal voltage offset for the analog comparator, the
     *  MCU used actually U_bandgap + U_offset. We get that offset by
     *  comparing the bandgap reference with the voltage of the cap:
     *  U_c = U_bandgap + U_offset -> U_offset = U_c - U_bandgap
     */

    Offset = U_c - Ticks;          /* U_c - U_bandgap */

    /* limit offset to a valid range of -50mV - 50mV */
    if ((Offset > -50) && (Offset < 50))
    {
      NV.CompOffset = Offset;      /* update offset */
    }
  }

  return Flag;
}

#endif



/* ************************************************************************
 *   clean-up of local constants
 * ************************************************************************ */


/* source management */
#undef CAP_C



/* ************************************************************************
 *   EOF
 * ************************************************************************ */
