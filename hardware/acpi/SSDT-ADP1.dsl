/*
 * Firebat U6C: the AC adapter for macOS.
 *
 * The firmware's \_SB.ADP1._PSR also reports AC/DC changes to AMD ALIB, and under macOS the method fails as a
 * whole, so SMCBatteryManager saw the adapter as unplugged while the battery charged. OpenCore renames that
 * method to XPSR (ACPI -> Patch, DSDT "_PSR" -> "XPSR", the only occurrence); this _PSR reads the EC bit
 * directly on Darwin and keeps the firmware path for other systems.
 */
DefinitionBlock ("", "SSDT", 2, "DRIP", "ADP1PSR", 0x00000000)
{
    External (_SB_.ADP1, DeviceObj)
    External (_SB_.ADP1.XPSR, MethodObj)
    External (_SB_.PCI0.SBRG.EC0_.ACST, FieldUnitObj)

    Scope (_SB.ADP1)
    {
        Method (_PSR, 0, NotSerialized)  // _PSR: Power Source
        {
            If (_OSI ("Darwin"))
            {
                Return (\_SB.PCI0.SBRG.EC0.ACST)
            }

            Return (XPSR ())
        }
    }
}
