/*
  ============================================================
  Brady DS2431 Automatic Refill Tool
  ============================================================

  Supported BRC media formats:

  1) DIE-CUT / SLEEVES
     -----------------
     Remaining:
       0x78 = LSB
       0x79 = MSB

       uint16 label count

     Full capacity:
       Find marker:

         3C 00 50 00

       Capacity is uint16 little-endian at:

         marker + 0x0A


  2) CONTINUOUS / M6C
     ----------------
     Identified by:
       BRC format
       record type 0x06
       part number beginning "M6C-"

     Remaining length:
       0x78 = byte 0
       0x79 = byte 1
       0x7A = byte 2

       uint24 little-endian

     Unit:
       0.001 inch

     Full length:
       static addresses 0x49 - 0x4B
       uint24 little-endian
       unit = 0.001 inch

     Verified example:

       M6C-1900-439-YL

       C0 27 09
       = 600000
       = 600.000"
       = 50 ft
       = 15.24 m


  Dynamic checksum for both formats:

       0x7F =
         sum(0x78 ... 0x7E) modulo 256


  Static Brady CRC:

       data:
         0x00 - 0x75

       stored CRC:
         0x76 - 0x77

       Maxim/Dallas CRC16
       inverted
       little-endian


  ============================================================
  SAFETY
  ============================================================

  Refill requires:

    - DS2431 family code 0x2D
    - valid ROM CRC
    - same physical ROM still connected
    - BRC header
    - valid static Brady CRC
    - recognized media layout
    - valid existing dynamic checksum
    - Page 3 = OPEN (0x83 = 0x00)
    - scratchpad verification
    - EEPROM read-back verification

  Legacy / non-BRC Smart Cells:
    READ ONLY

  Unknown BRC layouts:
    READ ONLY

  This program NEVER writes protection registers.

  ============================================================
*/

#include <OneWire.h>
#include <string.h>


// ============================================================
// HARDWARE
// ============================================================

OneWire ds(2);

const int SWITCH_PIN = 5;


// ============================================================
// OPTIONS
// ============================================================

const bool PRINT_FULL_DUMP_ON_INSERT = true;


// ============================================================
// MEDIA TYPE
// ============================================================

enum MediaMode {

  MEDIA_UNSUPPORTED = 0,

  MEDIA_DIECUT,

  MEDIA_CONTINUOUS
};


// ============================================================
// ROLL INFO
// ============================================================

struct RollInfo {

  bool isBrc;

  bool staticCrcValid;

  byte recordType;

  char partNumber[40];

  MediaMode mediaMode;


  // ----------------------------------------------------------
  // Die-cut
  // ----------------------------------------------------------

  int capacityMarkerAddress;

  uint16_t capacityAddress;

  uint16_t fullLabels;


  // ----------------------------------------------------------
  // Continuous
  // ----------------------------------------------------------

  uint16_t fullLengthAddress;

  uint32_t fullLengthMil;
};


// ============================================================
// GLOBAL STATE
// ============================================================

byte chipRom[8];

bool rollPresent = false;

int switch_pos = HIGH;


// ============================================================
// LED
// ============================================================

void setColor(
  bool r,
  bool g,
  bool b
) {

  digitalWrite(
    LED_RED,
    r ? LOW : HIGH
  );

  digitalWrite(
    LED_GREEN,
    g ? LOW : HIGH
  );

  digitalWrite(
    LED_BLUE,
    b ? LOW : HIGH
  );
}


void ledRed() {

  setColor(
    true,
    false,
    false
  );
}


void ledGreen() {

  setColor(
    false,
    true,
    false
  );
}


void ledBlue() {

  setColor(
    false,
    false,
    true
  );
}


void ledYellow() {

  setColor(
    true,
    true,
    false
  );
}


void ledWhite() {

  setColor(
    true,
    true,
    true
  );
}


void ledMagenta() {

  setColor(
    true,
    false,
    true
  );
}


void ledOff() {

  setColor(
    false,
    false,
    false
  );
}


// ============================================================
// HEX HELPERS
// ============================================================

void printHexByte(
  byte value
) {

  if (value < 0x10) {

    Serial.print("0");
  }

  Serial.print(
    value,
    HEX
  );
}


void printHexWord(
  uint16_t value
) {

  if (value < 0x1000) Serial.print("0");
  if (value < 0x0100) Serial.print("0");
  if (value < 0x0010) Serial.print("0");

  Serial.print(
    value,
    HEX
  );
}


// ============================================================
// UINT24 HELPERS
// ============================================================

uint32_t readUint24LE(
  const byte* data
) {

  return
    ((uint32_t)data[2] << 16)
    |
    ((uint32_t)data[1] << 8)
    |
    data[0];
}


void writeUint24LE(
  byte* data,
  uint32_t value
) {

  data[0] =
    value & 0xFF;


  data[1] =
    (value >> 8) & 0xFF;


  data[2] =
    (value >> 16) & 0xFF;
}


// ============================================================
// ROM
// ============================================================

void printRom(
  byte rom[8]
) {

  Serial.print(
    "ROM: "
  );


  for (int i = 0; i < 8; i++) {

    printHexByte(
      rom[i]
    );

    Serial.print(" ");
  }


  Serial.println();
}


bool isDS2431(
  byte rom[8]
) {

  return
    rom[0] == 0x2D;
}


bool romCrcValid(
  byte rom[8]
) {

  return
    OneWire::crc8(
      rom,
      7
    )
    ==
    rom[7];
}


bool romEqual(
  const byte a[8],
  const byte b[8]
) {

  for (int i = 0; i < 8; i++) {

    if (a[i] != b[i]) {

      return false;
    }
  }


  return true;
}


// ============================================================
// CHECK SAME PHYSICAL ROLL
// ============================================================

bool sameRollStillPresent(
  byte expectedRom[8]
) {

  byte currentRom[8];


  ds.reset_search();


  if (!ds.search(
        currentRom
      )) {

    return false;
  }


  return
    romEqual(
      expectedRom,
      currentRom
    );
}


// ============================================================
// MATCH ROM
// ============================================================

bool resetAndMatch(
  byte rom[8]
) {

  if (!ds.reset()) {

    return false;
  }


  // Match ROM
  ds.write(
    0x55
  );


  for (int i = 0; i < 8; i++) {

    ds.write(
      rom[i]
    );
  }


  return true;
}


// ============================================================
// MEMORY READ
// ============================================================

bool readMemory(
  byte rom[8],
  uint16_t address,
  byte* buffer,
  int length
) {

  if (!resetAndMatch(
        rom
      )) {

    return false;
  }


  // Read Memory
  ds.write(
    0xF0
  );


  // TA1
  ds.write(
    lowByte(address)
  );


  // TA2
  ds.write(
    highByte(address)
  );


  for (int i = 0; i < length; i++) {

    buffer[i] =
      ds.read();
  }


  return true;
}


// ============================================================
// PAGE READ
// ============================================================

bool readPage(
  byte rom[8],
  uint16_t address,
  byte buffer[32]
) {

  return readMemory(
    rom,
    address,
    buffer,
    32
  );
}


// ============================================================
// CONTROL BYTES
// ============================================================

bool readControlBytes(
  byte rom[8],
  byte buffer[8]
) {

  return readMemory(
    rom,
    0x0080,
    buffer,
    8
  );
}


// ============================================================
// MAXIM / DALLAS CRC16
// ============================================================

uint16_t maximCrc16(
  const byte* data,
  int length
) {

  uint16_t crc =
    0x0000;


  for (int i = 0; i < length; i++) {

    crc ^=
      data[i];


    for (int bit = 0; bit < 8; bit++) {

      if (crc & 0x0001) {

        crc =
          (crc >> 1)
          ^
          0xA001;
      }

      else {

        crc >>=
          1;
      }
    }
  }


  return crc;
}


// ============================================================
// STATIC BRADY CRC
// ============================================================

bool staticBradyCrcValid(
  byte staticData[0x78]
) {

  uint16_t calculated =
    maximCrc16(
      staticData,
      0x76
    );


  calculated =
    ~calculated;


  uint16_t stored =
    ((uint16_t)staticData[0x77] << 8)
    |
    staticData[0x76];


  return
    calculated == stored;
}


// ============================================================
// FIND DIE-CUT CAPACITY
//
// Marker:
//
//   3C 00 50 00
//
// Capacity:
//
//   marker + 0x0A
//
// ============================================================

bool findDieCutCapacity(
  byte staticData[0x78],
  int &markerAddress,
  uint16_t &capacityAddress,
  uint16_t &capacity
) {

  const byte marker[4] = {

    0x3C,
    0x00,
    0x50,
    0x00
  };


  int foundAddress =
    -1;


  int foundCount =
    0;


  for (
    int address = 0;
    address <= 0x6A;
    address++
  ) {

    bool match =
      true;


    for (int i = 0; i < 4; i++) {

      if (
        staticData[address + i]
        !=
        marker[i]
      ) {

        match =
          false;

        break;
      }
    }


    if (match) {

      foundAddress =
        address;

      foundCount++;
    }
  }


  // Must be unique
  if (foundCount != 1) {

    return false;
  }


  markerAddress =
    foundAddress;


  capacityAddress =
    foundAddress + 0x0A;


  capacity =
    ((uint16_t)staticData[
        capacityAddress + 1
      ] << 8)
    |
    staticData[
      capacityAddress
    ];


  if (
    capacity == 0
    ||
    capacity == 0xFFFF
  ) {

    return false;
  }


  return true;
}


// ============================================================
// READ ROLL INFO
// ============================================================

bool readRollInfo(
  byte rom[8],
  RollInfo &info
) {

  byte staticData[0x78];


  memset(
    &info,
    0,
    sizeof(info)
  );


  info.mediaMode =
    MEDIA_UNSUPPORTED;


  info.capacityMarkerAddress =
    -1;


  // ----------------------------------------------------------
  // Read 0x00 - 0x77
  // ----------------------------------------------------------

  if (!readMemory(
        rom,
        0x0000,
        staticData,
        0x78
      )) {

    return false;
  }


  // ----------------------------------------------------------
  // Static CRC
  // ----------------------------------------------------------

  info.staticCrcValid =
    staticBradyCrcValid(
      staticData
    );


  // ----------------------------------------------------------
  // BRC
  // ----------------------------------------------------------

  info.isBrc =
    staticData[0] == 0x42
    &&
    staticData[1] == 0x52
    &&
    staticData[2] == 0x43;


  if (!info.isBrc) {

    return true;
  }


  // ----------------------------------------------------------
  // Record type
  // ----------------------------------------------------------

  info.recordType =
    staticData[0x06];


  // ----------------------------------------------------------
  // Part number
  //
  // Observed BRC start = 0x11
  // ----------------------------------------------------------

  int outputIndex =
    0;


  for (
    int address = 0x11;
    address <= 0x75;
    address++
  ) {

    byte value =
      staticData[address];


    if (value == 0x00) {

      break;
    }


    if (
      outputIndex
      <
      (int)sizeof(info.partNumber) - 1
    ) {

      info.partNumber[
        outputIndex
      ] =
        (char)value;


      outputIndex++;
    }
  }


  info.partNumber[
    outputIndex
  ] =
    '\0';


  // ==========================================================
  // CONTINUOUS BRC / M6C
  //
  // Verified:
  //
  // record type = 0x06
  // part number begins "M6C-"
  //
  // static full length:
  //     0x49 - 0x4B
  //
  // uint24 LE
  // unit = 0.001 inch
  // ==========================================================

  if (
    info.recordType == 0x06
    &&
    strncmp(
      info.partNumber,
      "M6C-",
      4
    ) == 0
  ) {

    info.mediaMode =
      MEDIA_CONTINUOUS;


    info.fullLengthAddress =
      0x49;


    info.fullLengthMil =
      readUint24LE(
        &staticData[
          info.fullLengthAddress
        ]
      );


    if (
      info.fullLengthMil == 0
      ||
      info.fullLengthMil == 0xFFFFFF
    ) {

      info.mediaMode =
        MEDIA_UNSUPPORTED;
    }


    return true;
  }


  // ==========================================================
  // DIE-CUT / SLEEVES
  // ==========================================================

  int markerAddress =
    -1;


  uint16_t capacityAddress =
    0;


  uint16_t capacity =
    0;


  if (
    findDieCutCapacity(
      staticData,
      markerAddress,
      capacityAddress,
      capacity
    )
  ) {

    info.mediaMode =
      MEDIA_DIECUT;


    info.capacityMarkerAddress =
      markerAddress;


    info.capacityAddress =
      capacityAddress;


    info.fullLabels =
      capacity;
  }


  return true;
}


// ============================================================
// DYNAMIC CHECKSUM
//
// 0x7F = sum(0x78 ... 0x7E)
// ============================================================

byte calculateDynamicChecksum(
  byte row[8]
) {

  byte checksum =
    0;


  for (int i = 0; i < 7; i++) {

    checksum +=
      row[i];
  }


  return checksum;
}


// ============================================================
// READ DYNAMIC ROW
// ============================================================

bool readDynamicRow(
  byte rom[8],
  byte row[8]
) {

  return readMemory(
    rom,
    0x0078,
    row,
    8
  );
}


// ============================================================
// PRINT LENGTH
//
// Input:
//   thousandths of an inch
// ============================================================

void printLength(
  uint32_t mil
) {

  float inches =
    mil / 1000.0f;


  float feet =
    inches / 12.0f;


  float meters =
    inches * 0.0254f;


  Serial.print(
    mil
  );

  Serial.println(
    " mil"
  );


  Serial.print(
    "                  "
  );

  Serial.print(
    inches,
    3
  );

  Serial.println(
    " in"
  );


  Serial.print(
    "                  "
  );

  Serial.print(
    feet,
    3
  );

  Serial.println(
    " ft"
  );


  Serial.print(
    "                  "
  );

  Serial.print(
    meters,
    3
  );

  Serial.println(
    " m"
  );
}


// ============================================================
// DISPLAY DYNAMIC DATA
// ============================================================

bool displayDynamicData(
  byte rom[8],
  RollInfo &info
) {

  byte row[8];


  if (!readDynamicRow(
        rom,
        row
      )) {

    Serial.println(
      "ERROR: Could not read dynamic row."
    );

    return false;
  }


  byte calculated =
    calculateDynamicChecksum(
      row
    );


  Serial.println();


  // ==========================================================
  // DIE-CUT
  // ==========================================================

  if (
    info.mediaMode
    ==
    MEDIA_DIECUT
  ) {

    uint16_t remaining =
      ((uint16_t)row[1] << 8)
      |
      row[0];


    Serial.print(
      "Labels remaining: "
    );


    Serial.println(
      remaining
    );
  }


  // ==========================================================
  // CONTINUOUS
  // ==========================================================

  else if (
    info.mediaMode
    ==
    MEDIA_CONTINUOUS
  ) {

    uint32_t remaining =
      readUint24LE(
        row
      );


    Serial.println(
      "Remaining length:"
    );


    Serial.print(
      "                  "
    );


    printLength(
      remaining
    );


    if (
      info.fullLengthMil > 0
    ) {

      float percent =
        (
          (float)remaining
          /
          (float)info.fullLengthMil
        )
        *
        100.0f;


      Serial.print(
        "Remaining percent: "
      );


      Serial.print(
        percent,
        1
      );


      Serial.println(
        "%"
      );
    }
  }


  else {

    Serial.println(
      "Dynamic data layout: UNSUPPORTED"
    );
  }


  // ----------------------------------------------------------
  // Raw row
  // ----------------------------------------------------------

  Serial.print(
    "Dynamic row 0x78-0x7F: "
  );


  for (int i = 0; i < 8; i++) {

    printHexByte(
      row[i]
    );

    Serial.print(" ");
  }


  Serial.println();


  // ----------------------------------------------------------
  // Checksum
  // ----------------------------------------------------------

  Serial.print(
    "Dynamic checksum stored: 0x"
  );


  printHexByte(
    row[7]
  );


  Serial.println();


  Serial.print(
    "Dynamic checksum calc:   0x"
  );


  printHexByte(
    calculated
  );


  Serial.println();


  Serial.print(
    "Dynamic checksum: "
  );


  if (
    calculated == row[7]
  ) {

    Serial.println(
      "OK"
    );
  }

  else {

    Serial.println(
      "INVALID"
    );
  }


  return true;
}


// ============================================================
// PAGE 3 WRITABLE
// ============================================================

bool page3Writable(
  byte rom[8],
  bool printStatus = true
) {

  byte ctrl[8];


  if (!readControlBytes(
        rom,
        ctrl
      )) {

    if (printStatus) {

      Serial.println(
        "ERROR: Could not read protection bytes."
      );
    }


    return false;
  }


  if (printStatus) {

    Serial.print(
      "Page 3 protection byte: 0x"
    );


    printHexByte(
      ctrl[3]
    );


    Serial.println();
  }


  if (
    ctrl[3] == 0x00
  ) {

    if (printStatus) {

      Serial.println(
        "Page 3: OPEN / WRITABLE"
      );
    }


    return true;
  }


  if (
    ctrl[3] == 0x55
  ) {

    if (printStatus) {

      Serial.println(
        "Page 3: WRITE PROTECTED"
      );
    }


    return false;
  }


  if (
    ctrl[3] == 0xAA
  ) {

    if (printStatus) {

      Serial.println(
        "Page 3: EPROM MODE"
      );
    }


    return false;
  }


  if (printStatus) {

    Serial.println(
      "Page 3: UNKNOWN PROTECTION STATE"
    );
  }


  return false;
}


// ============================================================
// PRINT FULL EEPROM
// ============================================================

void printAllPages(
  byte rom[8]
) {

  Serial.println();


  printRom(
    rom
  );


  for (
    int page = 0;
    page < 4;
    page++
  ) {

    byte buffer[32];


    uint16_t address =
      page * 0x20;


    if (!readPage(
          rom,
          address,
          buffer
        )) {

      Serial.println(
        "ERROR: EEPROM read failed."
      );

      return;
    }


    Serial.print(
      "Address 0x"
    );


    Serial.print(
      address,
      HEX
    );


    Serial.print(": ");


    for (int i = 0; i < 32; i++) {

      printHexByte(
        buffer[i]
      );

      Serial.print(" ");
    }


    Serial.println();
  }


  byte ctrl[8];


  if (!readControlBytes(
        rom,
        ctrl
      )) {

    Serial.println(
      "ERROR: Could not read control bytes."
    );

    return;
  }


  Serial.print(
    "Control Bytes 0x80 - 0x87: "
  );


  for (int i = 0; i < 8; i++) {

    printHexByte(
      ctrl[i]
    );

    Serial.print(" ");
  }


  Serial.println();


  Serial.println(
    "Page lock status:"
  );


  for (int i = 0; i < 4; i++) {

    Serial.print(
      "  Page "
    );


    Serial.print(i);


    Serial.print(": ");


    if (
      ctrl[i] == 0x55
    ) {

      Serial.println(
        "WRITE PROTECTED"
      );
    }

    else if (
      ctrl[i] == 0xAA
    ) {

      Serial.println(
        "EPROM MODE"
      );
    }

    else if (
      ctrl[i] == 0x00
    ) {

      Serial.println(
        "OPEN"
      );
    }

    else {

      Serial.print(
        "UNKNOWN (0x"
      );


      printHexByte(
        ctrl[i]
      );


      Serial.println(")");
    }
  }
}


// ============================================================
// CHECK ROLL READY
// ============================================================

bool rollReadyForRefill(
  byte rom[8]
) {

  if (!isDS2431(
        rom
      )) {

    return false;
  }


  if (!romCrcValid(
        rom
      )) {

    return false;
  }


  RollInfo info;


  if (!readRollInfo(
        rom,
        info
      )) {

    return false;
  }


  if (!info.isBrc) {

    return false;
  }


  if (!info.staticCrcValid) {

    return false;
  }


  if (
    info.mediaMode
    ==
    MEDIA_UNSUPPORTED
  ) {

    return false;
  }


  byte row[8];


  if (!readDynamicRow(
        rom,
        row
      )) {

    return false;
  }


  if (
    calculateDynamicChecksum(
      row
    )
    !=
    row[7]
  ) {

    return false;
  }


  if (!page3Writable(
        rom,
        false
      )) {

    return false;
  }


  return true;
}


// ============================================================
// WRITE COMPLETE DYNAMIC ROW
//
// Target:
//   0x78 - 0x7F
// ============================================================

bool writeDynamicRow(
  byte rom[8],
  byte newRow[8]
) {

  // ==========================================================
  // SAME PHYSICAL ROLL
  // ==========================================================

  if (!sameRollStillPresent(
        rom
      )) {

    Serial.println(
      "ERROR: Connected ROM changed."
    );


    return false;
  }


  // ==========================================================
  // WRITE SCRATCHPAD
  // ==========================================================

  ledYellow();


  if (!resetAndMatch(
        rom
      )) {

    Serial.println(
      "ERROR: Roll disappeared before Write Scratchpad."
    );


    return false;
  }


  ds.write(
    0x0F
  );


  // Target 0x0078
  ds.write(
    0x78
  );


  ds.write(
    0x00
  );


  for (int i = 0; i < 8; i++) {

    ds.write(
      newRow[i]
    );
  }


  // ==========================================================
  // READ SCRATCHPAD
  // ==========================================================

  if (!resetAndMatch(
        rom
      )) {

    Serial.println(
      "ERROR: Roll disappeared before Read Scratchpad."
    );


    return false;
  }


  ds.write(
    0xAA
  );


  byte TA1 =
    ds.read();


  byte TA2 =
    ds.read();


  byte ES =
    ds.read();


  byte verify[8];


  for (int i = 0; i < 8; i++) {

    verify[i] =
      ds.read();
  }


  Serial.print(
    "Scratchpad TA1: 0x"
  );


  printHexByte(
    TA1
  );


  Serial.println();


  Serial.print(
    "Scratchpad TA2: 0x"
  );


  printHexByte(
    TA2
  );


  Serial.println();


  Serial.print(
    "Scratchpad E/S: 0x"
  );


  printHexByte(
    ES
  );


  Serial.println();


  // ----------------------------------------------------------
  // Address
  // ----------------------------------------------------------

  if (
    TA1 != 0x78
    ||
    TA2 != 0x00
  ) {

    Serial.println(
      "ERROR: Scratchpad address mismatch."
    );


    return false;
  }


  // ----------------------------------------------------------
  // PF
  // ----------------------------------------------------------

  if (
    ES & 0x20
  ) {

    Serial.println(
      "ERROR: Scratchpad PF flag set."
    );


    return false;
  }


  // ----------------------------------------------------------
  // AA
  // ----------------------------------------------------------

  if (
    ES & 0x80
  ) {

    Serial.println(
      "ERROR: Scratchpad AA flag unexpectedly set."
    );


    return false;
  }


  // ----------------------------------------------------------
  // Ending offset
  // ----------------------------------------------------------

  if (
    (ES & 0x07)
    !=
    0x07
  ) {

    Serial.println(
      "ERROR: Scratchpad ending offset is not 7."
    );


    return false;
  }


  // ----------------------------------------------------------
  // Data
  // ----------------------------------------------------------

  for (int i = 0; i < 8; i++) {

    if (
      verify[i]
      !=
      newRow[i]
    ) {

      Serial.print(
        "ERROR: Scratchpad mismatch at byte "
      );


      Serial.println(i);


      return false;
    }
  }


  Serial.println(
    "Scratchpad verification: OK"
  );


  // ==========================================================
  // SAME ROM AGAIN
  // ==========================================================

  if (!sameRollStillPresent(
        rom
      )) {

    Serial.println(
      "ERROR: Roll changed before EEPROM programming."
    );


    return false;
  }


  // ==========================================================
  // COPY SCRATCHPAD
  // ==========================================================

  if (!resetAndMatch(
        rom
      )) {

    Serial.println(
      "ERROR: Roll disappeared before Copy Scratchpad."
    );


    return false;
  }


  ds.write(
    0x55
  );


  // Authorization
  ds.write(
    TA1
  );


  ds.write(
    TA2
  );


  // Keep 1-Wire powered during EEPROM programming
  ds.write(
    ES,
    1
  );


  delay(
    15
  );


  ds.depower();


  // ==========================================================
  // EEPROM VERIFY
  // ==========================================================

  byte finalRow[8];


  if (!readDynamicRow(
        rom,
        finalRow
      )) {

    Serial.println(
      "ERROR: Could not read EEPROM after programming."
    );


    return false;
  }


  for (int i = 0; i < 8; i++) {

    if (
      finalRow[i]
      !=
      newRow[i]
    ) {

      Serial.print(
        "ERROR: EEPROM verification failed at 0x"
      );


      Serial.println(
        0x78 + i,
        HEX
      );


      return false;
    }
  }


  Serial.println(
    "EEPROM verification: OK"
  );


  return true;
}


// ============================================================
// REFILL
// ============================================================

bool refillRollToFull(
  byte rom[8]
) {

  Serial.println();


  Serial.println(
    "================================"
  );


  Serial.println(
    " REFILL ROLL TO FULL"
  );


  Serial.println(
    "================================"
  );


  // ==========================================================
  // BASIC DEVICE CHECKS
  // ==========================================================

  if (!isDS2431(
        rom
      )) {

    Serial.println(
      "ERROR: Not DS2431."
    );


    ledRed();


    return false;
  }


  if (!romCrcValid(
        rom
      )) {

    Serial.println(
      "ERROR: ROM CRC invalid."
    );


    ledRed();


    return false;
  }


  if (!sameRollStillPresent(
        rom
      )) {

    Serial.println(
      "ERROR: Original roll is no longer connected."
    );


    ledRed();


    return false;
  }


  // ==========================================================
  // DECODE
  // ==========================================================

  RollInfo info;


  if (!readRollInfo(
        rom,
        info
      )) {

    Serial.println(
      "ERROR: Could not read roll information."
    );


    ledRed();


    return false;
  }


  if (!info.isBrc) {

    Serial.println(
      "ERROR: Non-BRC / legacy Smart Cell."
    );


    Serial.println(
      "READ ONLY."
    );


    ledBlue();


    return false;
  }


  if (!info.staticCrcValid) {

    Serial.println(
      "ERROR: Static Brady CRC invalid."
    );


    ledRed();


    return false;
  }


  if (
    info.mediaMode
    ==
    MEDIA_UNSUPPORTED
  ) {

    Serial.println(
      "ERROR: Unsupported BRC layout."
    );


    Serial.println(
      "Refill cancelled."
    );


    ledBlue();


    return false;
  }


  // ==========================================================
  // PAGE 3
  // ==========================================================

  if (!page3Writable(
        rom,
        true
      )) {

    Serial.println(
      "ERROR: Page 3 cannot be modified."
    );


    ledRed();


    return false;
  }


  // ==========================================================
  // CURRENT DYNAMIC ROW
  // ==========================================================

  byte row[8];


  if (!readDynamicRow(
        rom,
        row
      )) {

    Serial.println(
      "ERROR: Could not read dynamic row."
    );


    ledRed();


    return false;
  }


  if (
    calculateDynamicChecksum(
      row
    )
    !=
    row[7]
  ) {

    Serial.println(
      "ERROR: Existing dynamic checksum invalid."
    );


    ledRed();


    return false;
  }


  // ==========================================================
  // DIE-CUT REFILL
  // ==========================================================

  if (
    info.mediaMode
    ==
    MEDIA_DIECUT
  ) {

    uint16_t current =
      ((uint16_t)row[1] << 8)
      |
      row[0];


    Serial.println(
      "Media type: DIE-CUT / COUNT BASED"
    );


    Serial.print(
      "Part number: "
    );


    Serial.println(
      info.partNumber
    );


    Serial.print(
      "Current remaining: "
    );


    Serial.println(
      current
    );


    Serial.print(
      "Full capacity: "
    );


    Serial.println(
      info.fullLabels
    );


    if (
      current
      ==
      info.fullLabels
    ) {

      Serial.println(
        "ROLL IS ALREADY FULL."
      );


      Serial.println(
        "No EEPROM write required."
      );


      ledGreen();


      return true;
    }


    // --------------------------------------------------------
    // Set uint16 counter
    // --------------------------------------------------------

    row[0] =
      lowByte(
        info.fullLabels
      );


    row[1] =
      highByte(
        info.fullLabels
      );


    // row[2] - row[6] preserved


    row[7] =
      calculateDynamicChecksum(
        row
      );


    Serial.print(
      "New row: "
    );


    for (int i = 0; i < 8; i++) {

      printHexByte(
        row[i]
      );

      Serial.print(" ");
    }


    Serial.println();
  }


  // ==========================================================
  // CONTINUOUS REFILL
  // ==========================================================

  else if (
    info.mediaMode
    ==
    MEDIA_CONTINUOUS
  ) {

    uint32_t current =
      readUint24LE(
        row
      );


    Serial.println(
      "Media type: CONTINUOUS"
    );


    Serial.print(
      "Part number: "
    );


    Serial.println(
      info.partNumber
    );


    Serial.println(
      "Current remaining:"
    );


    Serial.print(
      "                  "
    );


    printLength(
      current
    );


    Serial.println(
      "Full roll length:"
    );


    Serial.print(
      "                  "
    );


    printLength(
      info.fullLengthMil
    );


    if (
      current
      ==
      info.fullLengthMil
    ) {

      Serial.println(
        "ROLL IS ALREADY FULL."
      );


      Serial.println(
        "No EEPROM write required."
      );


      ledGreen();


      return true;
    }


    // --------------------------------------------------------
    // Set uint24 remaining length
    //
    // 0x78 - 0x7A
    // --------------------------------------------------------

    writeUint24LE(
      row,
      info.fullLengthMil
    );


    // row[3] - row[6] preserved


    row[7] =
      calculateDynamicChecksum(
        row
      );


    Serial.print(
      "New row: "
    );


    for (int i = 0; i < 8; i++) {

      printHexByte(
        row[i]
      );

      Serial.print(" ");
    }


    Serial.println();
  }


  // ==========================================================
  // WRITE
  // ==========================================================

  if (!writeDynamicRow(
        rom,
        row
      )) {

    Serial.println();


    Serial.println(
      "REFILL WRITE FAILED."
    );


    ledRed();


    return false;
  }


  // ==========================================================
  // SUCCESS
  // ==========================================================

  Serial.println();


  Serial.println(
    "================================"
  );


  Serial.println(
    " REFILL SUCCESSFUL"
  );


  Serial.println(
    "================================"
  );


  RollInfo finalInfo;


  if (
    readRollInfo(
      rom,
      finalInfo
    )
  ) {

    displayDynamicData(
      rom,
      finalInfo
    );
  }


  ledGreen();


  return true;
}


// ============================================================
// HANDLE NEW ROLL
// ============================================================

void handleNewRoll() {

  Serial.println();
  Serial.println();


  Serial.println(
    "================================"
  );


  Serial.println(
    " NEW ROLL DETECTED"
  );


  Serial.println(
    "================================"
  );


  printRom(
    chipRom
  );


  // ==========================================================
  // DEVICE
  // ==========================================================

  Serial.print(
    "Device type: "
  );


  if (
    isDS2431(
      chipRom
    )
  ) {

    Serial.println(
      "DS2431"
    );
  }

  else {

    Serial.println(
      "NOT DS2431"
    );


    ledBlue();


    return;
  }


  // ==========================================================
  // ROM CRC
  // ==========================================================

  Serial.print(
    "ROM CRC: "
  );


  if (
    romCrcValid(
      chipRom
    )
  ) {

    Serial.println(
      "OK"
    );
  }

  else {

    Serial.println(
      "INVALID"
    );


    ledRed();


    return;
  }


  // ==========================================================
  // RAW EEPROM DUMP
  // ==========================================================

  if (
    PRINT_FULL_DUMP_ON_INSERT
  ) {

    printAllPages(
      chipRom
    );
  }


  // ==========================================================
  // DECODE
  // ==========================================================

  RollInfo info;


  if (!readRollInfo(
        chipRom,
        info
      )) {

    Serial.println(
      "ERROR: Could not read Smart Cell."
    );


    ledRed();


    return;
  }


  Serial.println();


  // ==========================================================
  // NON-BRC
  // ==========================================================

  if (!info.isBrc) {

    Serial.println(
      "Brady BRC format: NOT FOUND"
    );


    Serial.print(
      "Static CRC: "
    );


    Serial.println(
      info.staticCrcValid
      ?
      "OK"
      :
      "INVALID"
    );


    Serial.println();


    Serial.println(
      "LEGACY / UNSUPPORTED SMART CELL"
    );


    Serial.println(
      "READ ONLY - REFILL DISABLED"
    );


    ledBlue();


    return;
  }


  // ==========================================================
  // BRC INFO
  // ==========================================================

  Serial.println(
    "Brady BRC format: OK"
  );


  Serial.print(
    "Part number: "
  );


  Serial.println(
    info.partNumber
  );


  Serial.print(
    "Record type: 0x"
  );


  printHexByte(
    info.recordType
  );


  Serial.println();


  Serial.print(
    "Static CRC: "
  );


  Serial.println(
    info.staticCrcValid
    ?
    "OK"
    :
    "INVALID"
  );


  // ==========================================================
  // DIE-CUT
  // ==========================================================

  if (
    info.mediaMode
    ==
    MEDIA_DIECUT
  ) {

    Serial.println(
      "Media type: DIE-CUT / COUNT BASED"
    );


    Serial.print(
      "Capacity marker address: 0x"
    );


    Serial.println(
      info.capacityMarkerAddress,
      HEX
    );


    Serial.print(
      "Capacity address: 0x"
    );


    Serial.println(
      info.capacityAddress,
      HEX
    );


    Serial.print(
      "Nominal / full capacity: "
    );


    Serial.println(
      info.fullLabels
    );
  }


  // ==========================================================
  // CONTINUOUS
  // ==========================================================

  else if (
    info.mediaMode
    ==
    MEDIA_CONTINUOUS
  ) {

    Serial.println(
      "Media type: CONTINUOUS"
    );


    Serial.print(
      "Full length address: 0x"
    );


    Serial.println(
      info.fullLengthAddress,
      HEX
    );


    Serial.println(
      "Nominal / full roll length:"
    );


    Serial.print(
      "                  "
    );


    printLength(
      info.fullLengthMil
    );
  }


  // ==========================================================
  // UNKNOWN BRC
  // ==========================================================

  else {

    Serial.println(
      "Media type: UNKNOWN BRC LAYOUT"
    );


    Serial.println(
      "REFILL DISABLED"
    );
  }


  // ==========================================================
  // DYNAMIC DATA
  // ==========================================================

  displayDynamicData(
    chipRom,
    info
  );


  // ==========================================================
  // PAGE 3
  // ==========================================================

  Serial.println();


  page3Writable(
    chipRom,
    true
  );


  // ==========================================================
  // READY
  // ==========================================================

  Serial.println();


  if (
    rollReadyForRefill(
      chipRom
    )
  ) {

    Serial.println(
      "--------------------------------"
    );


    Serial.println(
      "ROLL READY FOR REFILL"
    );


    if (
      info.mediaMode
      ==
      MEDIA_DIECUT
    ) {

      Serial.print(
        "Flip switch to refill to "
      );


      Serial.print(
        info.fullLabels
      );


      Serial.println(
        " labels."
      );
    }


    else if (
      info.mediaMode
      ==
      MEDIA_CONTINUOUS
    ) {

      Serial.print(
        "Flip switch to refill continuous roll to "
      );


      Serial.print(
        info.fullLengthMil / 1000.0f,
        3
      );


      Serial.println(
        " inches."
      );
    }


    Serial.println(
      "--------------------------------"
    );


    ledGreen();
  }


  else {

    Serial.println(
      "--------------------------------"
    );


    Serial.println(
      "ROLL CANNOT BE AUTOMATICALLY REFILLED"
    );


    Serial.println(
      "See status above for reason."
    );


    Serial.println(
      "--------------------------------"
    );


    ledBlue();
  }
}


// ============================================================
// SEARCH FOR ROLL
// ============================================================

bool searchForRoll() {

  byte rom[8];


  ds.reset_search();


  if (!ds.search(
        rom
      )) {

    return false;
  }


  for (int i = 0; i < 8; i++) {

    chipRom[i] =
      rom[i];
  }


  return true;
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
    9600
  );


  delay(
    500
  );


  pinMode(
    LED_RED,
    OUTPUT
  );


  pinMode(
    LED_GREEN,
    OUTPUT
  );


  pinMode(
    LED_BLUE,
    OUTPUT
  );


  pinMode(
    SWITCH_PIN,
    INPUT_PULLUP
  );


  // Prevent power-on write
  switch_pos =
    digitalRead(
      SWITCH_PIN
    );


  ledWhite();


  delay(
    1500
  );


  Serial.println();


  Serial.println(
    "================================"
  );


  Serial.println(
    " Brady DS2431 Automatic"
  );


  Serial.println(
    " BRC Refill Tool"
  );


  Serial.println(
    "================================"
  );


  Serial.println();


  Serial.println(
    "Supported:"
  );


  Serial.println(
    "  - BRC die-cut / sleeves"
  );


  Serial.println(
    "  - BRC M6C continuous"
  );


  Serial.println();


  Serial.println(
    "Legacy / non-BRC:"
  );


  Serial.println(
    "  READ ONLY"
  );


  Serial.println();


  Serial.println(
    "Waiting for label roll..."
  );


  ledYellow();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ==========================================================
  // NO ROLL
  // ==========================================================

  if (!rollPresent) {

    if (
      searchForRoll()
    ) {

      rollPresent =
        true;


      // Synchronize physical switch.
      // Inserting a roll can never trigger refill.
      switch_pos =
        digitalRead(
          SWITCH_PIN
        );


      ledMagenta();


      delay(
        500
      );


      handleNewRoll();
    }

    else {

      ledYellow();


      delay(
        250
      );
    }


    return;
  }


  // ==========================================================
  // ROLL REMOVED
  // ==========================================================

  if (!ds.reset()) {

    Serial.println();


    Serial.println(
      "Roll removed."
    );


    Serial.println(
      "Waiting for next roll..."
    );


    rollPresent =
      false;


    ds.reset_search();


    ledYellow();


    delay(
      500
    );


    return;
  }


  // ==========================================================
  // SWITCH
  // ==========================================================

  int current =
    digitalRead(
      SWITCH_PIN
    );


  if (
    current != switch_pos
  ) {

    // Debounce
    delay(
      50
    );


    current =
      digitalRead(
        SWITCH_PIN
      );


    if (
      current != switch_pos
    ) {

      // One physical flip = one attempt
      switch_pos =
        current;


      Serial.println();


      Serial.println(
        "SWITCH ACTIVATED"
      );


      // ======================================================
      // SAME ROLL CHECK
      // ======================================================

      if (!sameRollStillPresent(
            chipRom
          )) {

        Serial.println(
          "ERROR: Connected roll changed."
        );


        Serial.println(
          "Refill cancelled."
        );


        rollPresent =
          false;


        ds.reset_search();


        ledRed();


        delay(
          500
        );


        return;
      }


      // ======================================================
      // REFILL
      // ======================================================

      bool success =
        refillRollToFull(
          chipRom
        );


      if (success) {

        ledGreen();
      }

      else {

        Serial.println();


        Serial.println(
          "REFILL FAILED / CANCELLED"
        );


        ledRed();
      }
    }
  }


  delay(
    100
  );
}