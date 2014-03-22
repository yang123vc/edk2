/** @file
  This driver effectuates OVMF's platform configuration settings and exposes
  them via HII.

  Copyright (C) 2014, Red Hat, Inc.
  Copyright (c) 2009 - 2014, Intel Corporation. All rights reserved.<BR>

  This program and the accompanying materials are licensed and made available
  under the terms and conditions of the BSD License which accompanies this
  distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS, WITHOUT
  WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HiiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiHiiServicesLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HiiConfigAccess.h>
#include <Guid/MdeModuleHii.h>
#include <Guid/OvmfPlatformConfig.h>

#include "Platform.h"
#include "PlatformConfig.h"

//
// The HiiAddPackages() library function requires that any controller (or
// image) handle, to be associated with the HII packages under installation, be
// "decorated" with a device path. The tradition seems to be a vendor device
// path.
//
// We'd like to associate our HII packages with the driver's image handle. The
// first idea is to use the driver image's device path. Unfortunately, loaded
// images only come with an EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL (not the
// usual EFI_DEVICE_PATH_PROTOCOL), ie. a different GUID. In addition, even the
// EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL interface may be NULL, if the image
// has been loaded from an "unnamed" memory source buffer.
//
// Hence let's just stick with the tradition -- use a dedicated vendor device
// path, with the driver's FILE_GUID.
//
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH       VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL End;
} PKG_DEVICE_PATH;
#pragma pack()

STATIC PKG_DEVICE_PATH mPkgDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8) (sizeof (VENDOR_DEVICE_PATH)     ),
        (UINT8) (sizeof (VENDOR_DEVICE_PATH) >> 8)
      }
    },
    EFI_CALLER_ID_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      (UINT8) (END_DEVICE_PATH_LENGTH     ),
      (UINT8) (END_DEVICE_PATH_LENGTH >> 8)
    }
  }
};

//
// The configuration interface between the HII engine (form display etc) and
// this driver.
//
STATIC EFI_HII_CONFIG_ACCESS_PROTOCOL mConfigAccess;

//
// The handle representing our list of packages after installation.
//
STATIC EFI_HII_HANDLE mInstalledPackages;

//
// The arrays below constitute our HII package list. They are auto-generated by
// the VFR compiler and linked into the driver image during the build.
//
// - The strings package receives its C identifier from the driver's BASE_NAME,
//   plus "Strings".
//
// - The forms package receives its C identifier from the VFR file's basename,
//   plus "Bin".
//
//
extern UINT8 PlatformDxeStrings[];
extern UINT8 PlatformFormsBin[];


/**
  This function is called by the HII machinery when it fetches the form state.

  See the precise documentation in the UEFI spec.

  @param[in]  This      The Config Access Protocol instance.

  @param[in]  Request   A <ConfigRequest> format UCS-2 string describing the
                        query.

  @param[out] Progress  A pointer into Request on output, identifying the query
                        element where processing failed.

  @param[out] Results   A <MultiConfigAltResp> format UCS-2 string that has
                        all values filled in for the names in the Request
                        string.

  @return  Status codes from gHiiConfigRouting->BlockToConfig().

**/
STATIC
EFI_STATUS
EFIAPI
ExtractConfig (
  IN CONST  EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN CONST  EFI_STRING                      Request,
  OUT       EFI_STRING                      *Progress,
  OUT       EFI_STRING                      *Results
)
{
  MAIN_FORM_STATE MainFormState;
  EFI_STATUS      Status;

  DEBUG ((EFI_D_VERBOSE, "%a: Request=\"%s\"\n", __FUNCTION__, Request));

  StrnCpy ((CHAR16 *) MainFormState.CurrentPreferredResolution,
           L"Unset", MAXSIZE_RES_CUR);
  MainFormState.NextPreferredResolution = 0;
  Status = gHiiConfigRouting->BlockToConfig (gHiiConfigRouting, Request,
                                (VOID *) &MainFormState, sizeof MainFormState,
                                Results, Progress);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: BlockToConfig(): %r, Progress=\"%s\"\n",
      __FUNCTION__, Status, (Status == EFI_DEVICE_ERROR) ? NULL : *Progress));
  } else {
    DEBUG ((EFI_D_VERBOSE, "%a: Results=\"%s\"\n", __FUNCTION__, *Results));
  }
  return Status;
}


STATIC
EFI_STATUS
EFIAPI
RouteConfig (
  IN CONST  EFI_HII_CONFIG_ACCESS_PROTOCOL  *This,
  IN CONST  EFI_STRING                      Configuration,
  OUT       EFI_STRING                      *Progress
)
{
  return EFI_SUCCESS;
}


STATIC
EFI_STATUS
EFIAPI
Callback (
  IN     CONST EFI_HII_CONFIG_ACCESS_PROTOCOL   *This,
  IN     EFI_BROWSER_ACTION                     Action,
  IN     EFI_QUESTION_ID                        QuestionId,
  IN     UINT8                                  Type,
  IN OUT EFI_IFR_TYPE_VALUE                     *Value,
  OUT    EFI_BROWSER_ACTION_REQUEST             *ActionRequest
  )
{
  return EFI_SUCCESS;
}


/**
  Create a set of "one-of-many" (ie. "drop down list") option IFR opcodes,
  based on available GOP resolutions, to be placed under a "one-of-many" (ie.
  "drop down list") opcode.

  @param[in]  PackageList   The package list with the formset and form for
                            which the drop down options are produced. Option
                            names are added as new strings to PackageList.

  @param[out] OpCodeBuffer  On output, a dynamically allocated opcode buffer
                            with drop down list options corresponding to GOP
                            resolutions. The caller is responsible for freeing
                            OpCodeBuffer with HiiFreeOpCodeHandle() after use.

  @retval EFI_SUCESS  Opcodes have been successfully produced.

  @return             Status codes from underlying functions. PackageList may
                      have been extended with new strings. OpCodeBuffer is
                      unchanged.
**/
STATIC
EFI_STATUS
EFIAPI
CreateResolutionOptions (
  IN  EFI_HII_HANDLE  *PackageList,
  OUT VOID            **OpCodeBuffer
  )
{
  EFI_STATUS                   Status;
  VOID                         *OutputBuffer;
  EFI_STRING_ID                NewString;
  VOID                         *OpCode;

  OutputBuffer = HiiAllocateOpCodeHandle ();
  if (OutputBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewString = HiiSetString (PackageList, 0 /* new string */, L"800x600",
                NULL /* for all languages */);
  if (NewString == 0) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeOutputBuffer;
  }
  OpCode = HiiCreateOneOfOptionOpCode (OutputBuffer, NewString,
             0 /* Flags */, EFI_IFR_NUMERIC_SIZE_4, 0 /* Value */);
  if (OpCode == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeOutputBuffer;
  }

  *OpCodeBuffer = OutputBuffer;
  return EFI_SUCCESS;

FreeOutputBuffer:
  HiiFreeOpCodeHandle (OutputBuffer);

  return Status;
}


/**
  Populate the form identified by the (PackageList, FormSetGuid, FormId)
  triplet.

  @retval EFI_SUCESS  Form successfully updated.
  @return             Status codes from underlying functions.

**/
STATIC
EFI_STATUS
EFIAPI
PopulateForm (
  IN  EFI_HII_HANDLE  *PackageList,
  IN  EFI_GUID        *FormSetGuid,
  IN  EFI_FORM_ID     FormId
  )
{
  EFI_STATUS         Status;
  VOID               *OpCodeBuffer;
  VOID               *OpCode;
  EFI_IFR_GUID_LABEL *Anchor;
  VOID               *OpCodeBuffer2;

  OpCodeBuffer2 = NULL;

  //
  // 1. Allocate an empty opcode buffer.
  //
  OpCodeBuffer = HiiAllocateOpCodeHandle ();
  if (OpCodeBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // 2. Create a label opcode (which is a Tiano extension) inside the buffer.
  // The label's number must match the "anchor" label in the form.
  //
  OpCode = HiiCreateGuidOpCode (OpCodeBuffer, &gEfiIfrTianoGuid,
             NULL /* optional copy origin */, sizeof *Anchor);
  if (OpCode == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeOpCodeBuffer;
  }
  Anchor               = OpCode;
  Anchor->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  Anchor->Number       = LABEL_RES_NEXT;

  //
  // 3. Create the opcodes inside the buffer that are to be inserted into the
  // form.
  //
  // 3.1. Get a list of resolutions.
  //
  Status = CreateResolutionOptions (PackageList, &OpCodeBuffer2);
  if (EFI_ERROR (Status)) {
    goto FreeOpCodeBuffer;
  }

  //
  // 3.2. Create a one-of-many question with the above options.
  //
  OpCode = HiiCreateOneOfOpCode (
             OpCodeBuffer,                        // create opcode inside this
                                                  //   opcode buffer,
             QUESTION_RES_NEXT,                   // ID of question,
             FORMSTATEID_MAIN_FORM,               // identifies form state
                                                  //   storage,
             (UINT16) OFFSET_OF (MAIN_FORM_STATE, // value of question stored
                        NextPreferredResolution), //   at this offset,
             STRING_TOKEN (STR_RES_NEXT),         // Prompt,
             STRING_TOKEN (STR_RES_NEXT_HELP),    // Help,
             0,                                   // QuestionFlags,
             EFI_IFR_NUMERIC_SIZE_4,              // see sizeof
                                                  //   NextPreferredResolution,
             OpCodeBuffer2,                       // buffer with possible
                                                  //   choices,
             NULL                                 // DEFAULT opcodes
             );
  if (OpCode == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeOpCodeBuffer2;
  }

  //
  // 4. Update the form with the opcode buffer.
  //
  Status = HiiUpdateForm (PackageList, FormSetGuid, FormId,
             OpCodeBuffer, // buffer with head anchor, and new contents to be
                           // inserted at it
             NULL          // buffer with tail anchor, for deleting old
                           // contents up to it
             );

FreeOpCodeBuffer2:
  HiiFreeOpCodeHandle (OpCodeBuffer2);

FreeOpCodeBuffer:
  HiiFreeOpCodeHandle (OpCodeBuffer);

  return Status;
}


/**
  Load and execute the platform configuration.

  @retval EFI_SUCCESS            Configuration loaded and executed.
  @return                        Status codes from PlatformConfigLoad().
**/
STATIC
EFI_STATUS
EFIAPI
ExecutePlatformConfig (
  VOID
  )
{
  EFI_STATUS      Status;
  PLATFORM_CONFIG PlatformConfig;
  UINT64          OptionalElements;

  Status = PlatformConfigLoad (&PlatformConfig, &OptionalElements);
  if (EFI_ERROR (Status)) {
    DEBUG (((Status == EFI_NOT_FOUND) ? EFI_D_VERBOSE : EFI_D_ERROR,
      "%a: failed to load platform config: %r\n", __FUNCTION__, Status));
    return Status;
  }

  if (OptionalElements & PLATFORM_CONFIG_F_GRAPHICS_RESOLUTION) {
    //
    // Pass the preferred resolution to GraphicsConsoleDxe via dynamic PCDs.
    //
    PcdSet32 (PcdVideoHorizontalResolution,
      PlatformConfig.HorizontalResolution);
    PcdSet32 (PcdVideoVerticalResolution,
      PlatformConfig.VerticalResolution);
  }

  return EFI_SUCCESS;
}


/**
  Entry point for this driver.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  Pointer to SystemTable.

  @retval EFI_SUCESS            Driver has loaded successfully.
  @retval EFI_OUT_OF_RESOURCES  Failed to install HII packages.
  @return                       Error codes from lower level functions.

**/
EFI_STATUS
EFIAPI
PlatformInit (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  ExecutePlatformConfig ();

  mConfigAccess.ExtractConfig = &ExtractConfig;
  mConfigAccess.RouteConfig   = &RouteConfig;
  mConfigAccess.Callback      = &Callback;

  //
  // Declare ourselves suitable for HII communication.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle,
                  &gEfiDevicePathProtocolGuid,      &mPkgDevicePath,
                  &gEfiHiiConfigAccessProtocolGuid, &mConfigAccess,
                  NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Publish the HII package list to HII Database.
  //
  mInstalledPackages = HiiAddPackages (
                         &gEfiCallerIdGuid,  // PackageListGuid
                         ImageHandle,        // associated DeviceHandle
                         PlatformDxeStrings, // 1st package
                         PlatformFormsBin,   // 2nd package
                         NULL                // terminator
                         );
  if (mInstalledPackages == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto UninstallProtocols;
  }

  Status = PopulateForm (mInstalledPackages, &gOvmfPlatformConfigGuid,
             FORMID_MAIN_FORM);
  if (EFI_ERROR (Status)) {
    goto RemovePackages;
  }

  return EFI_SUCCESS;

RemovePackages:
  HiiRemovePackages (mInstalledPackages);

UninstallProtocols:
  gBS->UninstallMultipleProtocolInterfaces (ImageHandle,
         &gEfiDevicePathProtocolGuid,      &mPkgDevicePath,
         &gEfiHiiConfigAccessProtocolGuid, &mConfigAccess,
         NULL);
  return Status;
}

/**
  Unload the driver.

  @param[in]  ImageHandle  Handle that identifies the image to evict.

  @retval EFI_SUCCESS  The image has been unloaded.
**/
EFI_STATUS
EFIAPI
PlatformUnload (
  IN  EFI_HANDLE  ImageHandle
  )
{
  HiiRemovePackages (mInstalledPackages);
  gBS->UninstallMultipleProtocolInterfaces (ImageHandle,
         &gEfiDevicePathProtocolGuid,      &mPkgDevicePath,
         &gEfiHiiConfigAccessProtocolGuid, &mConfigAccess,
         NULL);
  return EFI_SUCCESS;
}
