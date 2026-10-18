"""Dump a RenderDoc capture of the native renderer: the action list, and a PNG of every copy destination and of the swap chain at the end.

Run inside RenderDoc's own Python (it has no standalone module on Windows):
  qrenderdoc.exe --python tools/renderdoc/dump_capture.py
with the capture path and output folder in the environment:
  CHERRY_RDC=<capture.rdc> CHERRY_RDC_OUT=<folder>
and optionally CHERRY_RDC_DRAW_EVERY=<n> to also save the colour target after every nth draw, and CHERRY_RDC_ALPHA=1 to keep alpha in the PNGs.
"""

import os
import sys

import renderdoc as rd

capture_path = os.environ["CHERRY_RDC"]
out_dir = os.environ["CHERRY_RDC_OUT"]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "actions.txt"), "w")


def save(controller, resource, name):
    texsave = rd.TextureSave()
    texsave.resourceId = resource
    texsave.destType = rd.FileType.PNG
    texsave.mip = 0
    texsave.slice.sliceIndex = 0
    texsave.alpha = rd.AlphaMapping.Preserve if os.environ.get("CHERRY_RDC_ALPHA") else rd.AlphaMapping.Discard
    controller.SaveTexture(texsave, os.path.join(out_dir, name))


DRAW_EVERY = int(os.environ.get("CHERRY_RDC_DRAW_EVERY", "0"))


def walk(controller, actions, depth, counter):
    for action in actions:
        if DRAW_EVERY and action.flags & rd.ActionFlags.Drawcall and action.outputs[0] != rd.ResourceId.Null():
            counter[1] += 1
            if counter[1] % DRAW_EVERY == 0:
                controller.SetFrameEvent(action.eventId, True)
                save(controller, action.outputs[0], "draw_{:05}_eid{}.png".format(counter[1], action.eventId))
        flags = action.flags
        outputs = [o for o in action.outputs if o != rd.ResourceId.Null()]
        log.write("{}{} eid {} flags {} outputs {} copyDest {}\n".format("  " * depth, action.GetName(controller.GetStructuredFile()), action.eventId, int(flags), [str(o) for o in outputs], str(action.copyDestination)))
        if flags & rd.ActionFlags.Copy and action.copyDestination != rd.ResourceId.Null():
            controller.SetFrameEvent(action.eventId, True)
            counter[0] += 1
            save(controller, action.copyDestination, "{:03}_copy_eid{}.png".format(counter[0], action.eventId))
        walk(controller, action.children, depth + 1, counter)


def main():
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(capture_path, "", None)
    if result != rd.ResultCode.Succeeded:
        log.write("open failed: {}\n".format(result))
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("replay failed: {}\n".format(result))
        return
    counter = [0, 0]
    walk(controller, controller.GetRootActions(), 0, counter)
    # The last action's outputs at the end of the frame: the swap chain image after the blit.
    actions = controller.GetRootActions()
    if actions:
        last = actions[-1]
        controller.SetFrameEvent(last.eventId, True)
        for i, texture in enumerate(controller.GetTextures()):
            if texture.creationFlags & rd.TextureCategory.SwapBuffer:
                save(controller, texture.resourceId, "zz_swapchain_{}.png".format(i))
    controller.Shutdown()
    cap.Shutdown()
    log.write("done, {} copies\n".format(counter[0]))


try:
    main()
except Exception as e:
    log.write("error: {}\n".format(e))
log.close()
os._exit(0)
