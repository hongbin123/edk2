/** @file

  Stateful and implicitly initialized fw_cfg library implementation.

  Copyright (C) 2013, Red Hat, Inc.
  Copyright (c) 2011 - 2013, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2017, Advanced Micro Devices. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/QemuFwCfgLib.h>
#include <WorkArea.h>

#include "QemuFwCfgLibInternal.h"

/**
  Check if it is Tdx guest

  @retval    TRUE   It is Tdx guest
  @retval    FALSE  It is not Tdx guest
**/
STATIC
BOOLEAN
QemuFwCfgIsCcGuest (
  VOID
  )
{
  CONFIDENTIAL_COMPUTING_WORK_AREA_HEADER  *CcWorkAreaHeader;

  CcWorkAreaHeader = (CONFIDENTIAL_COMPUTING_WORK_AREA_HEADER *)FixedPcdGet32 (PcdOvmfWorkAreaBase);
  return (CcWorkAreaHeader != NULL && CcWorkAreaHeader->GuestType != CcGuestTypeNonEncrypted);
}

/**
  Returns a boolean indicating if the firmware configuration interface
  is available or not.

  This function may change fw_cfg state.

  @retval    TRUE   The interface is available
  @retval    FALSE  The interface is not available

**/
BOOLEAN
EFIAPI
QemuFwCfgIsAvailable (
  VOID
  )
{
  return InternalQemuFwCfgIsAvailable ();
}

STATIC
VOID
QemuFwCfgProbe (
  BOOLEAN  *Supported,
  BOOLEAN  *DmaSupported
  )
{
  UINT32   Signature;
  UINT32   Revision;
  BOOLEAN  CcGuest;

  // Use direct Io* calls for probing to avoid recursion.
  QemuFwCfgSelectItem (QemuFwCfgItemSignature);
  if ( InternalQemuFwCfgCacheReading ()) {
    InternalQemuFwCfgCacheReadBytes (sizeof Signature, &Signature);
  } else {
    IoReadFifo8 (FW_CFG_IO_DATA, sizeof Signature, &Signature);
  }

  QemuFwCfgSelectItem (QemuFwCfgItemInterfaceVersion);
  if ( InternalQemuFwCfgCacheReading ()) {
    InternalQemuFwCfgCacheReadBytes (sizeof Revision, &Revision);
  } else {
    IoReadFifo8 (FW_CFG_IO_DATA, sizeof Revision, &Revision);
  }

  CcGuest = QemuFwCfgIsCcGuest ();

  *Supported    = FALSE;
  *DmaSupported = FALSE;
  if ((Signature == SIGNATURE_32 ('Q', 'E', 'M', 'U')) && (Revision >= 1)) {
    *Supported = TRUE;
    if ((Revision & FW_CFG_F_DMA) && !CcGuest) {
      *DmaSupported = TRUE;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: Supported %d, DMA %d\n",
    __func__,
    *Supported,
    *DmaSupported
    ));
}

STATIC
EFI_HOB_PLATFORM_INFO *
QemuFwCfgGetPlatformInfo (
  VOID
  )
{
  EFI_HOB_PLATFORM_INFO  *PlatformInfoHob;
  EFI_HOB_GUID_TYPE      *GuidHob;

  GuidHob = GetFirstGuidHob (&gUefiOvmfPkgPlatformInfoGuid);
  if (GuidHob == NULL) {
    return NULL;
  }

  PlatformInfoHob = (EFI_HOB_PLATFORM_INFO *)GET_GUID_HOB_DATA (GuidHob);

  if (!PlatformInfoHob->QemuFwCfgChecked) {
    QemuFwCfgProbe (
      &PlatformInfoHob->QemuFwCfgWorkArea.QemuFwCfgSupported,
      &PlatformInfoHob->QemuFwCfgWorkArea.QemuFwCfgDmaSupported
      );
    PlatformInfoHob->QemuFwCfgChecked = TRUE;
  }

  return PlatformInfoHob;
}

RETURN_STATUS
EFIAPI
QemuFwCfgInitialize (
  VOID
  )
{
  return RETURN_SUCCESS;
}

/**
  Returns a boolean indicating if the firmware configuration interface is
  available for library-internal purposes.

  This function never changes fw_cfg state.

  @retval    TRUE   The interface is available internally.
  @retval    FALSE  The interface is not available internally.
**/
BOOLEAN
InternalQemuFwCfgIsAvailable (
  VOID
  )
{
  EFI_HOB_PLATFORM_INFO  *PlatformInfoHob = QemuFwCfgGetPlatformInfo ();

  return PlatformInfoHob->QemuFwCfgWorkArea.QemuFwCfgSupported;
}

/**
  Returns a boolean indicating whether QEMU provides the DMA-like access method
  for fw_cfg.

  @retval    TRUE   The DMA-like access method is available.
  @retval    FALSE  The DMA-like access method is unavailable.
**/
BOOLEAN
InternalQemuFwCfgDmaIsAvailable (
  VOID
  )
{
  EFI_HOB_PLATFORM_INFO  *PlatformInfoHob = QemuFwCfgGetPlatformInfo ();

  return PlatformInfoHob->QemuFwCfgWorkArea.QemuFwCfgDmaSupported;
}

/**
  Transfer an array of bytes, or skip a number of bytes, using the DMA
  interface.

  @param[in]     Size     Size in bytes to transfer or skip.

  @param[in,out] Buffer   Buffer to read data into or write data from. Ignored,
                          and may be NULL, if Size is zero, or Control is
                          FW_CFG_DMA_CTL_SKIP.

  @param[in]     Control  One of the following:
                          FW_CFG_DMA_CTL_WRITE - write to fw_cfg from Buffer.
                          FW_CFG_DMA_CTL_READ  - read from fw_cfg into Buffer.
                          FW_CFG_DMA_CTL_SKIP  - skip bytes in fw_cfg.
**/
VOID
InternalQemuFwCfgDmaBytes (
  IN     UINT32  Size,
  IN OUT VOID    *Buffer OPTIONAL,
  IN     UINT32  Control
  )
{
  volatile FW_CFG_DMA_ACCESS  Access;
  UINT32                      AccessHigh, AccessLow;
  UINT32                      Status;

  ASSERT (
    Control == FW_CFG_DMA_CTL_WRITE || Control == FW_CFG_DMA_CTL_READ ||
    Control == FW_CFG_DMA_CTL_SKIP
    );

  if (Size == 0) {
    return;
  }

  //
  // TDX does not support DMA operations in PEI stage, we should
  // not have reached here.
  //
  ASSERT (!QemuFwCfgIsCcGuest ());

  Access.Control = SwapBytes32 (Control);
  Access.Length  = SwapBytes32 (Size);
  Access.Address = SwapBytes64 ((UINTN)Buffer);

  //
  // Delimit the transfer from (a) modifications to Access, (b) in case of a
  // write, from writes to Buffer by the caller.
  //
  MemoryFence ();

  //
  // Start the transfer.
  //
  AccessHigh = (UINT32)RShiftU64 ((UINTN)&Access, 32);
  AccessLow  = (UINT32)(UINTN)&Access;
  IoWrite32 (FW_CFG_IO_DMA_ADDRESS, SwapBytes32 (AccessHigh));
  IoWrite32 (FW_CFG_IO_DMA_ADDRESS + 4, SwapBytes32 (AccessLow));

  //
  // Don't look at Access.Control before starting the transfer.
  //
  MemoryFence ();

  //
  // Wait for the transfer to complete.
  //
  do {
    Status = SwapBytes32 (Access.Control);
    ASSERT ((Status & FW_CFG_DMA_CTL_ERROR) == 0);
  } while (Status != 0);

  //
  // After a read, the caller will want to use Buffer.
  //
  MemoryFence ();
}

/**
  Get the pointer to the QEMU_FW_CFG_WORK_AREA. This data is used as the
  workarea to record the onging fw_cfg item and offset.
  @retval   QEMU_FW_CFG_WORK_AREA  Pointer to the QEMU_FW_CFG_WORK_AREA
  @retval   NULL                QEMU_FW_CFG_WORK_AREA doesn't exist
**/
QEMU_FW_CFG_WORK_AREA *
InternalQemuFwCfgCacheGetWorkArea (
  VOID
  )
{
  EFI_HOB_GUID_TYPE      *GuidHob;
  EFI_HOB_PLATFORM_INFO  *PlatformHobinfo;

  GuidHob = GetFirstGuidHob (&gUefiOvmfPkgPlatformInfoGuid);
  if (GuidHob == NULL) {
    return NULL;
  }

  PlatformHobinfo = (EFI_HOB_PLATFORM_INFO *)(VOID *)GET_GUID_HOB_DATA (GuidHob);
  return &(PlatformHobinfo->QemuFwCfgWorkArea);
}

/**
 OVMF reads configuration data from QEMU via fw_cfg.
 For Td-Guest VMM is out of TCB and the configuration data is untrusted.
 From the security perpective the configuration data shall be measured
 before it is consumed.
 This function reads the fw_cfg items and cached them. In the meanwhile these
 fw_cfg items are measured as well. This is to avoid changing the order when
 reading the fw_cfg process, which depends on multiple factors(depex, order in
 the Firmware volume).
 @retval  RETURN_SUCCESS   - Successfully cache with measurement
 @retval  Others           - As the error code indicates
 */
RETURN_STATUS
EFIAPI
QemuFwCfgInitCache (
  IN OUT EFI_HOB_PLATFORM_INFO  *PlatformInfoHob
  )
{
  if (EFI_ERROR (InternalQemuFwCfgInitCache (PlatformInfoHob))) {
    return RETURN_ABORTED;
  }

  DEBUG ((DEBUG_INFO, "QemuFwCfgInitCache Pass!!!\n"));
  return RETURN_SUCCESS;
}
