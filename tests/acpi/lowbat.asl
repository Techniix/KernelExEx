/*
 * battest.asl: a simulated laptop for testing KEXACPI.VXD in QEMU
 * (qemu -acpitable file=lowbat.aml): one battery, nearly empty, for the warnings. Not shipped. GPLv2.
 *
 * BAT0: a modern battery (_BIX, mW/mWh), discharging; each _BST read takes
 *       500 mWh off, so the charge visibly drops.
 * BAT1: an older one (_BIF only, mA/mAh), charging; each read adds 50 mAh.
 * AC:   adapter, reports "on battery".
 * LID0: lid, open.
 */
DefinitionBlock ("", "SSDT", 2, "KEXEX", "LOWBAT", 1)
{
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 1)
            Name (CAP0, 6500)
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIX, 0)
            {
                Return (Package (21)
                {
                    1,          // revision
                    0,          // power unit: mW
                    50000,      // design capacity
                    45000,      // last full charge
                    1,          // rechargeable
                    11100,      // design voltage
                    2250, 1125, // warning, low
                    123,        // cycle count
                    100000,     // accuracy
                    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                    100, 100,   // granularity
                    "KEX-TEST-6C", "0042", "LION", "KernelExEx",
                    0           // swapping
                })
            }
            Method (_BST, 0)
            {
                If (CAP0 > 1000) { CAP0 -= 500 }
                Return (Package (4) { 1, 9000, CAP0, 11800 })
            }
        }

        Device (AC)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0) { Return (0) }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (1) }
        }
    }
}
