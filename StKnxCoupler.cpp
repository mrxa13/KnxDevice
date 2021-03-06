//    This file is part of Arduino Knx Bus Device library.

//    The Arduino Knx Bus Device library allows to turn Arduino into "self-made" KNX bus device.
//    Copyright (C) 2014 2015 2016 Franck MARINI (fm@liwan.fr)

//    The Arduino Knx Bus Device library is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.

//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.

//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.


// File : StKnxCoupler.cpp
// Author : Franz Auernigg
// Description : Communication with StKnxCoupler Chip
// Module dependencies : KnxTelegram, KnxComObject

#include "StKnxCoupler.h"

static inline word TimeDeltaWord(word now, word before) { return (word)(now - before); }

StKnxCoupler::StKnxCoupler(type_TransmitCallbackFctPtr cb, word physicalAddr,
  type_KnxBusCouplerMode mode) :
  _extTxCb(cb),
  _physicalAddr(physicalAddr),
  _mode(mode)
{
  _rx.state = RX_RESET;
  _rx.addressedComObjectIndex = 0;
  _tx.state = TX_RESET;
  _tx.sentTelegram = NULL;
  _tx.ackFctPtr = NULL;
  _tx.nbRemainingBytes = 0;
  _tx.txByteIndex = 0;
  _stateIndication = 0;
  _evtCallbackFct = NULL;
  _comObjectsList = NULL;
  _assignedComObjectsNb = 0;
  _orderedIndexTable = NULL;
  _stateIndication = 0;
#if defined(KNXTPUART_DEBUG_INFO) || defined(KNXTPUART_DEBUG_ERROR)
  _debugStrPtr = NULL;
#endif
}


// Destructor
StKnxCoupler::~StKnxCoupler()
{
  if (_orderedIndexTable) free(_orderedIndexTable);
  // close the serial communication if opened
  if ( (_rx.state > RX_RESET) || (_tx.state > TX_RESET) )
  {
#if defined(KNXTPUART_DEBUG_INFO)
    DebugInfo("Destructor: connection closed, byebye\n");
#endif
  }
#if defined(KNXTPUART_DEBUG_INFO)
  else DebugInfo("Desctructor: byebye\n");
#endif
}


// Reset implementation for stknx bus read thread
byte StKnxCoupler::Reset(void)
{
  _rx.state = RX_INIT;
  _tx.state = TX_INIT;

  return KNX_BUSCOUPLER_OK;
}


byte StKnxCoupler::AttachComObjectsList(KnxComObject comObjectsList[], byte listSize)
{
  return AttachComObjectsList(&comObjectsList, listSize);
}

// Attach a list of com objects
// NB1 : only the objects with "communication" attribute are considered by the TPUART
// NB2 : In case of objects with identical address, the object with highest index only is considered
// return KNX_BUSCOUPLER_ERROR_NOT_INIT_STATE (254) if the TPUART is not in Init state
// The function must be called prior to Init() execution
byte StKnxCoupler::AttachComObjectsList(KnxComObject** comObjectsList, byte listSize)
{
#define IS_COM(index) (comObjectsList[index]->GetIndicator() & KNX_COM_OBJ_C_INDICATOR)
#define ADDR(index) (comObjectsList[index]->GetAddr())

  if ((_rx.state!=RX_INIT) || (_tx.state!=TX_INIT)) return KNX_BUSCOUPLER_ERROR_NOT_INIT_STATE;

  if (_orderedIndexTable)
  {  // a list is already attached, we detach it
    free(_orderedIndexTable);
    _orderedIndexTable = NULL;
    _comObjectsList = NULL;
    _assignedComObjectsNb = 0;
  }
  if ((!comObjectsList) || (!listSize))
  {
#if defined(KNXTPUART_DEBUG_INFO)
    DebugInfo("AttachComObjectsList : warning : empty object list!\n");
#endif
    return  KNX_BUSCOUPLER_OK;
  }
  // Count all the com objects with communication indicator
  for (byte i=0; i < listSize ; i++) if (IS_COM(i)) _assignedComObjectsNb++;
  if (!_assignedComObjectsNb)
  {
#if defined(KNXTPUART_DEBUG_INFO)
    DebugInfo("AttachComObjectsList : warning : no object with com attribute in the list!\n");
#endif
    return  KNX_BUSCOUPLER_OK;
  }
  // Deduct the duplicate addresses
  for (byte i=0; i < listSize ; i++)
  {
    if (!IS_COM(i)) continue;
    for (byte j=0; j < listSize ; j++)
    {
      if ( (i!=j) && (ADDR(j) == ADDR(i)) && (IS_COM(j)) )
      { // duplicate address found
        if (j<i) break; // duplicate address already treated
        else
        {
          _assignedComObjectsNb--;
#if defined(KNXTPUART_DEBUG_INFO)
          DebugInfo("AttachComObjectsList : warning : duplicate address found!\n");
#endif
        }
      }
    }
  }
  _comObjectsList = comObjectsList;
  // Creation of the ordered index table
  _orderedIndexTable = (byte*) malloc(_assignedComObjectsNb);
  memset(_orderedIndexTable, 255, _assignedComObjectsNb);
  word minMin = 0x0000;   // minimum min value searched
  word foundMin = 0xFFFF; // min value found so far
  for (byte i=0; i < _assignedComObjectsNb; i++)
  {
    for (byte j=0; j < listSize ; j++)
    {
      if ( (IS_COM(j)) && (ADDR(j)>=minMin) && (ADDR(j)<=foundMin) )
      {
        foundMin = ADDR(j);
        _orderedIndexTable[i] = j;
      }
    }
    minMin = foundMin + 1;
    foundMin = 0xFFFF;
  }
#if defined(KNXTPUART_DEBUG_INFO)
  DebugInfo("AttachComObjectsList successful\n");
#endif
  return KNX_BUSCOUPLER_OK;
}


// Init for stknx
// Init must be called after every reset() execution
byte StKnxCoupler::Init(void)
{
  _rx.state = RX_IDLE_WAITING_FOR_CTRL_FIELD;
  _tx.state = TX_IDLE;

  return KNX_BUSCOUPLER_OK;
}


// Send a KNX telegram
// returns ERROR (255) if TX is not available, or if the telegram is not valid, else returns OK (0)
// NB : the source address is forced to TPUART physical address value
byte StKnxCoupler::SendTelegram(KnxTelegram& sentTelegram)
{
  if (_tx.state != TX_IDLE) return KNX_BUSCOUPLER_ERROR; // TX not initialized or busy

  if (sentTelegram.GetSourceAddress() != _physicalAddr) // Check that source addr equals TPUART physical addr
  { // if not, let's force source addr to the correct value
    sentTelegram.SetSourceAddress(_physicalAddr);
    sentTelegram.UpdateChecksum();
  }


  _tx.state = TX_IDLE;
  return _extTxCb(&sentTelegram);

  // TODO: test send later in TxTask
  _tx.sentTelegram = &sentTelegram;
  _tx.nbRemainingBytes = sentTelegram.GetTelegramLength();
  _tx.txByteIndex = 0; // Set index to 0
  _tx.state = TX_TELEGRAM_SENDING_ONGOING;

  return KNX_BUSCOUPLER_OK;
}

static KnxTelegram telegram; // telegram being received
static byte addressedComObjectIndex; // index of the com object targeted by the received telegram
static word lastByteRxTimeMicrosec;


void StKnxCoupler::SetReceivedTelegram(KnxTelegram &rxTelegram)
{
    if (IsAddressAssigned(rxTelegram.GetTargetAddress(), addressedComObjectIndex))
    { // Message addressed to us
      //rxTelegram.Copy(telegram);
      //_rx.state = RX_EIB_TELEGRAM_RECEPTION_ADDRESSED;

        if (rxTelegram.IsChecksumCorrect())
        { // checksum correct, let's update the _rx struct with the received telegram and correct index
          rxTelegram.Copy(_rx.receivedTelegram);
          _rx.addressedComObjectIndex  = addressedComObjectIndex;
          _evtCallbackFct(BUSCOUPLER_EVENT_RECEIVED_EIB_TELEGRAM); // Notify the new received telegram

          _rx.state = RX_IDLE_WAITING_FOR_CTRL_FIELD;
        }
    }
}

// Reception task
// This function shall be called periodically in order to allow a correct reception of the EIB bus data
// Assuming the TPUART speed is configured to 19200 baud, a character (8 data + 1 start + 1 parity + 1 stop)
// is transmitted in 0,58ms.
// In order not to miss any End Of Packets (i.e. a gap from 2 to 2,5ms), the function shall be called at a max period of 0,5ms.
// Typical calling period is 400 usec.
void StKnxCoupler::RXTask(void)
  {
  word nowTime;

  if (_extTxCb) {
    switch (_rx.state) {
      case RX_EIB_TELEGRAM_RECEPTION_ADDRESSED:
        if (telegram.IsChecksumCorrect())
        { // checksum correct, let's update the _rx struct with the received telegram and correct index
          telegram.Copy(_rx.receivedTelegram);
          _rx.addressedComObjectIndex  = addressedComObjectIndex;
          _evtCallbackFct(BUSCOUPLER_EVENT_RECEIVED_EIB_TELEGRAM); // Notify the new received telegram

          _rx.state = RX_IDLE_WAITING_FOR_CTRL_FIELD;
        }
        break;
      default: break;
    }
    return;
  }

// === STEP 1 : Check EOP in case a Telegram is being received ===
  if (_rx.state >= RX_EIB_TELEGRAM_RECEPTION_STARTED)
  { // a telegram reception is ongoing
    nowTime = (word) micros(); // word cast because a 65ms looping counter is long enough
    if(TimeDeltaWord(nowTime,lastByteRxTimeMicrosec) > 2000 /* 2 ms */ )
    { // EOP detected, the telegram reception is completed

      switch (_rx.state)
      {
        case RX_EIB_TELEGRAM_RECEPTION_STARTED : // we are not supposed to get EOP now, the telegram is incomplete
        case RX_EIB_TELEGRAM_RECEPTION_LENGTH_INVALID :
          _evtCallbackFct(BUSCOUPLER_EVENT_EIB_TELEGRAM_RECEPTION_ERROR); // Notify telegram reception error
          break;

        case RX_EIB_TELEGRAM_RECEPTION_ADDRESSED :
          if (telegram.IsChecksumCorrect())
          { // checksum correct, let's update the _rx struct with the received telegram and correct index
        	telegram.Copy(_rx.receivedTelegram);
            _rx.addressedComObjectIndex  = addressedComObjectIndex;
            _evtCallbackFct(BUSCOUPLER_EVENT_RECEIVED_EIB_TELEGRAM); // Notify the new received telegram
          }
          else
          {  // checksum incorrect, notify error
            _evtCallbackFct(BUSCOUPLER_EVENT_EIB_TELEGRAM_RECEPTION_ERROR); // Notify telegram reception error
          }
          break;

        // case RX_EIB_TELEGRAM_RECEPTION_NOT_ADDRESSED : break; // nothing to do!

        default : break;
      } // end of switch

      // we move state back to RX IDLE in any case
      _rx.state = RX_IDLE_WAITING_FOR_CTRL_FIELD;
    } // end EOP detected
  }
}


// Transmission task
// This function shall be called periodically in order to allow a correct transmission of the EIB bus data
// Assuming the TP-Uart speed is configured to 19200 baud, a character (8 data + 1 start + 1 parity + 1 stop)
// is transmitted in 0,58ms.
// Sending one byte of a telegram consists in transmitting 2 characters (1,16ms)
// Let's wait around 800us between each telegram piece sending so that the 64byte TX buffer remains almost empty.
// Typical calling period is 800 usec.
void StKnxCoupler::TXTask(void)
{
  // TODO handle trigger sending via txtask
  /*if (_tx.state == TX_TELEGRAM_SENDING_ONGOING) {
    (void) _extTxCb(_tx.sentTelegram);
    _tx.state = TX_IDLE;
    //_tx.state =  TX_TELEGRAM_SENDING_ONGOING;
    //_tx.state = TX_WAITING_ACK;
  }*/
  return;

  word nowTime;
  static word sentMessageTimeMillisec;

  // STEP 1 : Manage Message Acknowledge timeout
  switch (_tx.state)
  {
  case TX_WAITING_ACK :
    // A transmission ACK is awaited, increment Acknowledge timeout
    nowTime = (word) millis(); // word is enough to count up to 500
    if(TimeDeltaWord(nowTime,sentMessageTimeMillisec) > 500 /* 500 ms */ )
    { // The no-answer timeout value is defined as follows :
      // - The emission duration for a single max sized telegram is 40ms
      // - The telegram emission might be repeated 3 times (120ms)
      // - The telegram emission might be delayed by another message transmission ongoing
      // - The telegram emission might be delayed by the simultaneous transmission of higher prio messages
      // Let's take around 3 times the max emission duration (160ms) as arbitrary value
      _tx.ackFctPtr(NO_ANSWER_TIMEOUT); // Send a No Answer TIMEOUT
      _tx.state = TX_IDLE;
    }
    break;

  case TX_TELEGRAM_SENDING_ONGOING :
    // STEP 2 : send message if any to send
    // In case a telegram reception has just started, and the ACK has not been sent yet,
    // we block the transmission (for around 3,3ms) till the ACK is sent
    // In that way, the TX buffer will remain empty and the ACK will be sent immediately
    if (_rx.state != RX_EIB_TELEGRAM_RECEPTION_STARTED)
    {
      {
        if (_tx.nbRemainingBytes == 1)
        {

          // Message sending completed
          sentMessageTimeMillisec = (word)millis(); // memorize sending time in order to manage ACK timeout
	        _tx.state = TX_WAITING_ACK;
        }
        else
        {
          _tx.txByteIndex++;
          _tx.nbRemainingBytes--;
        }
      }
    }
    break;

  default : break;
  } // switch
}


// Get Bus monitoring data (BUS MONITORING mode)
// The function returns true if a new data has been retrieved (data pointer in argument), else false
// It shall be called periodically (max period of 0,5ms) in order to allow correct data reception
// Typical calling period is 400 usec.
boolean StKnxCoupler::GetMonitoringData(type_MonitorData& data)
{
  return true;
}


// Check if the target address is an assigned com object one
// if yes, then update index parameter with the index (in the list) of the targeted com object and return true
// else return false
boolean StKnxCoupler::IsAddressAssigned(word addr, byte &index) const
{
byte divisionCounter=0;
byte i, searchIndexStart, searchIndexStop, searchIndexRange;

  if (!_assignedComObjectsNb) return false; // in case of empty list, we return immediately

  // Define how many divisions by 2 shall be done in order to reduce the search list by 8 Addr max
  // if _assignedComObjectsNb >= 16 => divisionCounter = 1
  // if _assignedComObjectsNb >= 32 => divisionCounter = 2
  // if _assignedComObjectsNb >= 64 => divisionCounter = 3
  // if _assignedComObjectsNb >= 128 => divisionCounter = 4
  for (i=4; _assignedComObjectsNb >>i ; i++) divisionCounter++;

  // the starting point is to search on the whole address range (0 -> _assignedComObjectsNb -1)
  searchIndexStart = 0; searchIndexStop = _assignedComObjectsNb - 1; searchIndexRange = _assignedComObjectsNb;

  // reduce the address range if needed
  while(divisionCounter)
  {
    searchIndexRange>>=1; // Divide range width by 2
    if (_orderedIndexTable[searchIndexStart+searchIndexRange]!=255
    && addr >= _comObjectsList[_orderedIndexTable[searchIndexStart+searchIndexRange]]->GetAddr())
      searchIndexStart += searchIndexRange ;
    else searchIndexStop-=searchIndexRange;
    divisionCounter --;
  }

  // search the address value and index in the reduced range
  for (i = searchIndexStart;
      (_orderedIndexTable[i]!=255 && _orderedIndexTable[i]<=_assignedComObjectsNb &&
      _comObjectsList[_orderedIndexTable[i]]->GetAddr() != addr && i <= searchIndexStop);
      i++);
  if (i > searchIndexStop) return false; // Address is NOT part of the assigned addresses
  // Address is part of the assigned addresses
  index = _orderedIndexTable[i];
  return true;
}


// DEBUG purpose functions
void StKnxCoupler::DEBUG_SendResetCommand() { }

void StKnxCoupler::DEBUG_SendStateReqCommand() { }

//EOF
