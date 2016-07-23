/****************************************************************
Core class file for MiniGen board.

This code is beerware; if you use it, please buy me (or any other
SparkFun employee) a cold beverage next time you run into one of
us at the local.

2 Jan 2014- Mike Hord, SparkFun Electronics

Code developed in Arduino 1.0.5, on an Arduino Pro Mini 5V.

**Updated to Arduino 1.6.4 5/2015**

****************************************************************/
#include "SparkFun_MiniGen.h"
#include <SPI.h>

// ### Constructors ###

// Default constructor. Assumes that you've plopped the MiniGen onto a Pro
//  Mini Arduino and want to use the default chip select pin.
MiniGen::MiniGen()
{
  _FSYNCPin = 10;
  SPI.begin(); // Make sure the SPI subsystem is initialized.  Doesn't hurt to do this multiple times.
  pinMode( _FSYNCPin, OUTPUT);  // Make the FSYCPin (chip select) pin an output.
  digitalWrite( _FSYNCPin, HIGH);  // Disable the chip select initially.
}

// Overloaded constructor, for cases where the chip select pin is not
//  connected to the regular pin. Still assumes standard SPI connections.
MiniGen::MiniGen( int16_t FSYNCPin) // FIXME: Default Argument
{
  _FSYNCPin = FSYNCPin;
  SPI.begin(); // Make sure the SPI subsystem is initialized.  Doesn't hurt to do this multiple times.
  pinMode( _FSYNCPin, OUTPUT);  // Make the FSYCPin (chip select) pin an output.
  digitalWrite( _FSYNCPin, HIGH);  // Disable the chip select initially.
}

// ### Public Functions ###

// reset the AD part. This will disable all function generation and set the
//  output to approximately mid-level, constant voltage. Since we're resetting,
//  we can also forego worrying about maintaining the state of the other bits
//  in the config register.
void MiniGen::reset()
{
  uint32_t defaultFreq = freqCalc( 100.0);
  
  adjustFreq( FREQ0, FULL, defaultFreq);
  adjustFreq( FREQ1, FULL, defaultFreq);
  adjustPhaseShift( PHASE0, 0x0000);
  adjustPhaseShift( PHASE1, 0x0000);
  
  writeStart();
  writeData( 0x0100);
  writeData( 0x0000);
  writeEnd();
}

// Set the mode of the part. The mode (trinagle, sine, or square) is set by
//  three bits in the status register: D5 (OPBITEN), D3 (DIV2), and D1 (MODE).
//  Here's a nice truth table for those settings:
//  D5 D1 D3
//  0  0  x   Sine wave output
//  0  1  x   Triangle wave output
//  1  0  0   Square wave @ 1/2 frequency
//  1  0  1   Square wave @ frequency
//  1  1  x   Not allowed
void MiniGen::setMode( MODE newMode)
{
  // We want to adjust the three bits in the config register that we're
  //  interested in without screwing up anything else. Unfortunately, this
  //  part is write-only, so we need to maintain a local shadow, adjust that,
  //  then write it.
  configReg &= ~0x002A; // Clear D5, D3, and D1.
  // This switch statement sets the appropriate bit in the config register.
  switch(newMode)
  {
    case TRIANGLE:
      configReg |= 0x0002;
    break;
    case SQUARE_2:
      configReg |=0x0020;
    break;
    case SQUARE:
      configReg |=0x0028;
    break;
    case SINE:
      configReg |=0x0000;
    break;
  }
  
  writeStart();
  writeConfig( configReg);
  writeEnd();
}

// The AD9837 has two frequency registers that can be independently adjusted.
//  This allows us to fiddle with the value in one without affecting the output
//  of the device. The register used for calculating the output is selected by
//  toggling bit 11 of the config register.
void MiniGen::selectFreqReg( FREQREG reg)
{
  // For register FREQ0, we want to clear bit 11. Otherwise, set bit 11.
  if( reg == FREQ0) {
    configReg &= ~0x0800;
  } else {
    configReg |= 0x0800;
  }
  
  writeStart();
  writeConfig( configReg);
  writeEnd();
}

// Similarly, there are two phase registers, selected by bit 10 of the config
//  register.
void MiniGen::selectPhaseReg( PHASEREG reg)
{
  if( reg == PHASE0) {
    configReg &= ~0x0400;
  } else {
    configReg |= 0x0400;
  }
  
  writeStart();
  writeConfig( configReg);
  writeEnd();
}

// The frequency registers are 28 bits in size (combining the lower 14 bits of
//  two 16 bit writes; the upper 2 bits are the register address to write).
//  Bits 13 and 12 of the config register select how these writes are handled:
//  13 12
//  0  0   Any write to a frequency register is treated as a write to the lower
//          14 bits; this allows for fast fine adjustment.
//  0  1   Writes are send to upper 14 bits, allowing for fast coarse adjust.
//  1  x   First write of a pair goes to LSBs, second to MSBs. Note that the
//          user must, in this case, be certain to write in pairs, to avoid
//          unexpected results!
void MiniGen::setFreqAdjustMode( FREQADJUSTMODE newMode)
{
  // Start by clearing the bits in question.
  configReg &= ~0x3000;
  // Now, adjust the bits to match the truth table above.
  switch(newMode)
  {
    case COARSE:  // D13:12 = 01
      configReg |= 0x1000;
      break;
    case FINE:    // D13:12 = 00
      break;
    case FULL:    // D13:12 = 1x (we use 10)
    default:
      configReg |= 0x2000;
      break;
  }
  
  writeStart();
  writeConfig( configReg);
  writeEnd();
}

// The phase shift value is 12 bits long; it gets routed to the proper phase
//  register based on the value of the 3 MSBs (4th MSB is ignored).
void MiniGen::adjustPhaseShift( PHASEREG reg, uint16_t newPhase)
{
  writeStart();
  if( reg == PHASE0) {
      writePhase0( newPhase);
  } else {
      writePhase1( newPhase);
  }
  writeEnd();
}

// Okay, now we're going to handle frequency adjustments. This is a little
//  trickier than a phase adjust, because in addition to properly routing the
//  data, we need to know whether we're writing all 32 bits or just 16. I've
//  overloaded this function call for three cases: write with a mode change (if
//  one is needed), and write with the existing mode.

// Adjust the contents of the given register, and, if necessary, switch mode
//  to do so. This is probably the slowest method of updating a register.
void MiniGen::adjustFreq( FREQREG reg, FREQADJUSTMODE mode, uint32_t newFreq) {
  setFreqAdjustMode( mode);
  // Now, we can just call the normal 32-bit write.
  adjustFreq( reg, newFreq);
}

// Fine or coarse update of the given register; change modes if necessary to
//  do this.
void MiniGen::adjustFreq( FREQREG reg, FREQADJUSTMODE mode, uint16_t newFreq) {
  setFreqAdjustMode( mode);  // Set the mode
  adjustFreq( reg, newFreq); // Call the known-mode write.
}

// Adjust the contents of the register, but assume that the write mode is
//  already set to full. Note that if it is NOT set to full, bad things will
//  happen- the coarse or fine register will be updated with the contents of
//  the upper 14 bits of the 28 bits you *meant* to send.
void MiniGen::adjustFreq( FREQREG reg, uint32_t newFreq) {
  // We need to split the 32-bit input into two 16-bit values, then write those values into the correct frequency
  //   register.
  writeStart();
  if( reg == FREQ0) {
      writeFreq0( (uint16_t) newFreq);
      writeFreq0( (uint16_t) (newFreq>>14));
  } else {
      writeFreq1( (uint16_t) newFreq);
      writeFreq1( (uint16_t) (newFreq>>14));
  }
  writeEnd();
}

// Adjust the coarse or fine register, depending on the current mode. Note that
//  if the current adjust mode is FULL, this is going to cause undefined
//  behavior, as it will leave one transfer hanging. Maybe that means only
//  half the register gets loaded? Maybe nothing happens until another write
//  to that register? Either way, it's not going to be good.
void MiniGen::adjustFreq( FREQREG reg, uint16_t newFreq)
{
  writeStart();
  if( reg == FREQ0) {
      writeFreq0( newFreq);
  } else {
      writeFreq1( newFreq);
  }
  writeEnd();
}

// Helper function, used to calculate the integer value to be written to a
//  freq register for a desired output frequency.
// The output frequency is fclk/2^28 * FREQREG. For us, fclk is 16MHz. We can
//  save processor time by specifying a constant for fclk/2^28- .0596. That is,
//  in Hz, the smallest step size for adjusting the output frequency.
uint32_t MiniGen::freqCalc( float desiredFrequency)
{
  return (uint32_t) ( desiredFrequency / 0.0596);
}

// ### Private Functions ###

// writeStart() should be called at the beginning of each set of writes to prepare the SPI bus for writing.
void MiniGen::writeStart() {
  // BeginTransaction on the SPI bus.  AD9837 is rated to 40MHz bus clock, but limiting to 10MHz as that should be fast
  //   enough.  AD9837 also uses SPI Mode 2 (CPOL = 1, CPHA = 0).
  SPI.beginTransaction( SPISettings( 10000000, MSBFIRST, SPI_MODE2));
}

// writeData( data) should be called to write the data to the bus.  This will handle the chip select and writing of the
//   data.
void MiniGen::writeData( uint16_t data) {
  // Set the chip select pin to low to enable writing to the chip.
  digitalWrite( _FSYNCPin, LOW);
  
  // Write the data.
  SPI.transfer( (byte) (data>>8));
  SPI.transfer( (byte) data);
  
  // Set the chip select pin to high to disable writing to the chip.
  digitalWrite( _FSYNCPin, HIGH);
}

// writeEnd() should be called at the end of each set of writes to release the SPI bus for other uses.
void MiniGen::writeEnd() {
  // EndTransaction to release bus for other chips to use.
  SPI.endTransaction();
}

// writeConfig() sets the correct bits to write the config register.
void MiniGen::writeConfig( uint16_t data) {
  // Make sure to clear the top two bit to make sure we're writing the config register:
  data &= ~0xC000;
  
  writeData( data);
}

// writeConfig() sets the correct bits to write the config register.
void MiniGen::writeFreq0( uint16_t data) {
  // Make sure to clear the top two bit to make sure we're writing the config register:
  data &= ~0xC000;
  data |= 0x4000; // Set top two bits to 0b01.
  
  writeData( data);
}

// writeConfig() sets the correct bits to write the config register.
void MiniGen::writeFreq1( uint16_t data) {
  // Make sure to clear the top two bit to make sure we're writing the config register:
  data &= ~0xC000;
  data |= 0x8000; // Set top two bits to 0b10.
  
  writeData( data);
}

// writeConfig() sets the correct bits to write the config register.
void MiniGen::writePhase0( uint16_t data) {
  // Make sure to clear the top two bit to make sure we're writing the config register:
  data &= ~0xF000;
  data |= 0xC000; // Set top three bits to 0b110.
  
  writeData( data);
}

// writeConfig() sets the correct bits to write the config register.
void MiniGen::writePhase1( uint16_t data) {
  // Make sure to clear the top two bit to make sure we're writing the config register:
  data &= ~0xF000;
  data |= 0xE000; // Set top three bits to 0b111.
  
  writeData( data);
}
