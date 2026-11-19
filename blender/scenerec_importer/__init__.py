"""Scene Recorder importer: File > Import > Minecraft Scene Recording (.screc)."""
import os

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ImportHelper


class SCENEREC_OT_import(bpy.types.Operator, ImportHelper):
    """Import a recording made with the Scene Recorder Minecraft mod"""
    bl_idname = "scenerec.import_recording"
    bl_label = "Import Scene Recording"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".screc"
    filter_glob: StringProperty(default="*.screc", options={"HIDDEN"})
    set_cycles: BoolProperty(name="Use Cycles", description="Switch the render engine to Cycles", default=True)

    def execute(self, context):
        from . import importer
        path = self.filepath
        if os.path.isdir(path):
            path = os.path.join(path, "scene.screc")
        try:
            importer.import_recording(context, path, {"set_cycles": self.set_cycles})
        except Exception as e:  # report instead of a silent traceback in the console
            self.report({"ERROR"}, "Import failed: %s" % e)
            raise
        return {"FINISHED"}


def menu_func_import(self, context):
    self.layout.operator(SCENEREC_OT_import.bl_idname, text="Minecraft Scene Recording (.screc)")


def register():
    bpy.utils.register_class(SCENEREC_OT_import)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.utils.unregister_class(SCENEREC_OT_import)
