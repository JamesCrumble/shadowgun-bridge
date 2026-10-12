/*
 * Vendor GL extension stubs for the Mali-G31.
 *
 * libMassEffect.so imports 22 GL entry points from three vendor extensions -
 * NV_fence (Tegra), AMD_performance_monitor (Adreno/AMD) and QCOM_driver_control
 * (Adreno) - because the 2012 Android build shipped for every mobile GPU of the
 * era and links them unconditionally. The engine only ever CALLS them behind a
 * glGetString(GL_EXTENSIONS) guard, so a device that does not advertise the
 * extension never reaches them.
 *
 * The trap: they must still RESOLVE at load time or the loader refuses the
 * module. Under the harness they did, because desktop Mesa's libGL exports the
 * whole lot; on the console libmali.so.1 exports none of them, so the first
 * real boot died with "22 import(s) of the module have no implementation". The
 * harness could not see this - Mesa is more complete here than the Mali blob.
 *
 * So these are stubs whose only job is to exist. Where a call returns a count
 * it returns zero (no groups, no counters, no driver controls), which is what
 * an honest "this GPU has no such extension" looks like and keeps any code that
 * did slip past the guard from iterating over things that are not there. Fence
 * ids are handed out non-zero and TestFence reports "done" so a stray wait
 * completes instead of spinning. None of this runs in practice; it is the
 * airplane-mode of GL vendor extensions.
 */
#include <stdint.h>
#include <string.h>

#include "so_util.h"
#include "thunk_gen.h"

/* GL ABI scalar types, spelled out to avoid pulling a GL header into the
 * loader just for four typedefs. */
typedef int            GLsizei;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef char           GLchar;

#define GL_TRUE  1
#define GL_FALSE 0

extern "C" {

/* ---- NV_fence -------------------------------------------------------- */

ABI_ATTR static void gl_stub_GenFencesNV(GLsizei n, GLuint *fences)
{
    for (GLsizei i = 0; i < n && fences; i++)
        fences[i] = (GLuint)(i + 1);       /* non-zero, plausibly valid */
}
ABI_ATTR static void gl_stub_DeleteFencesNV(GLsizei n, const GLuint *fences)
{
    (void)n; (void)fences;
}
ABI_ATTR static void gl_stub_SetFenceNV(GLuint fence, GLenum condition)
{
    (void)fence; (void)condition;
}
ABI_ATTR static GLboolean gl_stub_TestFenceNV(GLuint fence)
{
    (void)fence;
    return GL_TRUE;                        /* always signalled: no waiting */
}
ABI_ATTR static void gl_stub_FinishFenceNV(GLuint fence)
{
    (void)fence;
}
ABI_ATTR static GLboolean gl_stub_IsFenceNV(GLuint fence)
{
    (void)fence;
    return GL_FALSE;
}
ABI_ATTR static void gl_stub_GetFenceivNV(GLuint fence, GLenum pname,
                                          GLint *params)
{
    (void)fence; (void)pname;
    if (params)
        *params = GL_TRUE;                 /* NV_FENCE_STATUS -> completed */
}

/* ---- AMD_performance_monitor ----------------------------------------- */

ABI_ATTR static void gl_stub_GetPerfMonitorGroupsAMD(GLint *numGroups,
                                                     GLsizei groupsSize,
                                                     GLuint *groups)
{
    (void)groupsSize; (void)groups;
    if (numGroups)
        *numGroups = 0;                    /* no monitor groups on this GPU */
}
ABI_ATTR static void gl_stub_GetPerfMonitorCountersAMD(
        GLuint group, GLint *numCounters, GLint *maxActiveCounters,
        GLsizei counterSize, GLuint *counters)
{
    (void)group; (void)counterSize; (void)counters;
    if (numCounters)       *numCounters = 0;
    if (maxActiveCounters) *maxActiveCounters = 0;
}
ABI_ATTR static void gl_stub_GetPerfMonitorGroupStringAMD(
        GLuint group, GLsizei bufSize, GLsizei *length, GLchar *groupString)
{
    (void)group; (void)bufSize;
    if (length)      *length = 0;
    if (groupString && bufSize > 0) groupString[0] = '\0';
}
ABI_ATTR static void gl_stub_GetPerfMonitorCounterStringAMD(
        GLuint group, GLuint counter, GLsizei bufSize, GLsizei *length,
        GLchar *counterString)
{
    (void)group; (void)counter; (void)bufSize;
    if (length)        *length = 0;
    if (counterString && bufSize > 0) counterString[0] = '\0';
}
ABI_ATTR static void gl_stub_GetPerfMonitorCounterInfoAMD(
        GLuint group, GLuint counter, GLenum pname, void *data)
{
    (void)group; (void)counter; (void)pname; (void)data;
}
ABI_ATTR static void gl_stub_GenPerfMonitorsAMD(GLsizei n, GLuint *monitors)
{
    for (GLsizei i = 0; i < n && monitors; i++)
        monitors[i] = (GLuint)(i + 1);
}
ABI_ATTR static void gl_stub_DeletePerfMonitorsAMD(GLsizei n, GLuint *monitors)
{
    (void)n; (void)monitors;
}
ABI_ATTR static void gl_stub_SelectPerfMonitorCountersAMD(
        GLuint monitor, GLboolean enable, GLuint group, GLint numCounters,
        GLuint *counterList)
{
    (void)monitor; (void)enable; (void)group; (void)numCounters;
    (void)counterList;
}
ABI_ATTR static void gl_stub_BeginPerfMonitorAMD(GLuint monitor)
{
    (void)monitor;
}
ABI_ATTR static void gl_stub_EndPerfMonitorAMD(GLuint monitor)
{
    (void)monitor;
}
ABI_ATTR static void gl_stub_GetPerfMonitorCounterDataAMD(
        GLuint monitor, GLenum pname, GLsizei dataSize, GLuint *data,
        GLint *bytesWritten)
{
    (void)monitor; (void)pname; (void)dataSize; (void)data;
    if (bytesWritten)
        *bytesWritten = 0;
}

/* ---- QCOM_driver_control --------------------------------------------- */

ABI_ATTR static void gl_stub_GetDriverControlsQCOM(GLint *num, GLsizei size,
                                                   GLuint *driverControls)
{
    (void)size; (void)driverControls;
    if (num)
        *num = 0;                          /* no driver controls exposed */
}
ABI_ATTR static void gl_stub_GetDriverControlStringQCOM(
        GLuint driverControl, GLsizei bufSize, GLsizei *length,
        GLchar *driverControlString)
{
    (void)driverControl; (void)bufSize;
    if (length)              *length = 0;
    if (driverControlString && bufSize > 0) driverControlString[0] = '\0';
}
ABI_ATTR static void gl_stub_EnableDriverControlQCOM(GLuint driverControl)
{
    (void)driverControl;
}
ABI_ATTR static void gl_stub_DisableDriverControlQCOM(GLuint driverControl)
{
    (void)driverControl;
}

}  /* extern "C" */

DynLibFunction symtable_gl_stubs[] = {
    THUNK_SPECIFIC("glGenFencesNV",                   gl_stub_GenFencesNV),
    THUNK_SPECIFIC("glDeleteFencesNV",                gl_stub_DeleteFencesNV),
    THUNK_SPECIFIC("glSetFenceNV",                    gl_stub_SetFenceNV),
    THUNK_SPECIFIC("glTestFenceNV",                   gl_stub_TestFenceNV),
    THUNK_SPECIFIC("glFinishFenceNV",                 gl_stub_FinishFenceNV),
    THUNK_SPECIFIC("glIsFenceNV",                     gl_stub_IsFenceNV),
    THUNK_SPECIFIC("glGetFenceivNV",                  gl_stub_GetFenceivNV),

    THUNK_SPECIFIC("glGetPerfMonitorGroupsAMD",       gl_stub_GetPerfMonitorGroupsAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCountersAMD",     gl_stub_GetPerfMonitorCountersAMD),
    THUNK_SPECIFIC("glGetPerfMonitorGroupStringAMD",  gl_stub_GetPerfMonitorGroupStringAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCounterStringAMD",gl_stub_GetPerfMonitorCounterStringAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCounterInfoAMD",  gl_stub_GetPerfMonitorCounterInfoAMD),
    THUNK_SPECIFIC("glGenPerfMonitorsAMD",            gl_stub_GenPerfMonitorsAMD),
    THUNK_SPECIFIC("glDeletePerfMonitorsAMD",         gl_stub_DeletePerfMonitorsAMD),
    THUNK_SPECIFIC("glSelectPerfMonitorCountersAMD",  gl_stub_SelectPerfMonitorCountersAMD),
    THUNK_SPECIFIC("glBeginPerfMonitorAMD",           gl_stub_BeginPerfMonitorAMD),
    THUNK_SPECIFIC("glEndPerfMonitorAMD",             gl_stub_EndPerfMonitorAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCounterDataAMD",  gl_stub_GetPerfMonitorCounterDataAMD),

    THUNK_SPECIFIC("glGetDriverControlsQCOM",         gl_stub_GetDriverControlsQCOM),
    THUNK_SPECIFIC("glGetDriverControlStringQCOM",    gl_stub_GetDriverControlStringQCOM),
    THUNK_SPECIFIC("glEnableDriverControlQCOM",       gl_stub_EnableDriverControlQCOM),
    THUNK_SPECIFIC("glDisableDriverControlQCOM",      gl_stub_DisableDriverControlQCOM),

    { NULL, 0 },
};
