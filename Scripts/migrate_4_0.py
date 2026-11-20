"""
InternetProtocol 3.x -> 4.0 Blueprint migration.

Loads, recompiles and re-saves every Blueprint, Widget Blueprint and map under the given content paths, so that
references to removed classes (UTCPRemoteSsl, UWSRemoteSsl, UHttpRemoteSsl -> the shared remote classes) are
written with the new names. The CoreRedirects in the plugin's Config/DefaultInternetProtocol.ini already make old
assets load; re-saving makes them independent of the redirects. Blueprints that still have compile errors are
listed in the log ("migrate_4_0: ERROR").

Run from the editor (Tools > Execute Python Script) after closing every open Blueprint editor, or headless:

    UnrealEditor-Cmd.exe <Project>.uproject -ExecutePythonScript="<plugin>/Scripts/migrate_4_0.py /Game"

Needs the Python Editor Script Plugin. Commit or back up your project first: assets are saved in place.
"""

import sys
import unreal

paths = [arg for arg in sys.argv[1:] if arg.startswith("/")] or ["/Game"]
registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.scan_paths_synchronous(paths, True)

errors = 0
saved = 0
for root in paths:
    for data in registry.get_assets_by_path(root, recursive=True):
        kind = str(data.asset_class_path.asset_name)
        package = str(data.package_name)
        if kind in ("Blueprint", "WidgetBlueprint"):
            blueprint = unreal.load_asset(package)
            unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
            status = blueprint.get_editor_property("status")
            if status == unreal.BlueprintStatus.BS_ERROR:
                errors += 1
                unreal.log_warning("migrate_4_0: ERROR %s does not compile; open it and fix the nodes shown." % package)
            if unreal.EditorAssetLibrary.save_loaded_asset(blueprint, only_if_is_dirty=False):
                saved += 1
        elif kind == "World":
            if unreal.EditorAssetLibrary.save_asset(package, only_if_is_dirty=False):
                saved += 1

unreal.log("migrate_4_0: %d assets saved, %d Blueprints with errors." % (saved, errors))
