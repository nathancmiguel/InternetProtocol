/**
 * Copyright © 2025 Nathan Miguel
 */

using UnrealBuildTool;

public class InternetProtocol : ModuleRules
{
	public InternetProtocol(ReadOnlyTargetRules Target) : base(Target)
	{
		if (Target.Version.MajorVersion == 5) {
            PublicDefinitions.Add("UE_ENABLE_RTTI=1");
            bUseRTTI = true;
			if (Target.Version.MinorVersion >= 6) {
                CppStandard = CppStandardVersion.Default;
            }
		}
		else {
			CppStandard = CppStandardVersion.Cpp17;
		}
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Asio configuration shared by every translation unit (and dependents including our headers).
		// Keeping it here guarantees all TUs see the same Asio ABI.
		PublicDefinitions.Add("ASIO_STANDALONE=1");
		PublicDefinitions.Add("ASIO_NO_EXCEPTIONS=1");
		PublicDefinitions.Add("ASIO_NO_DEPRECATED=1");

		PublicIncludePaths.AddRange(
			new string[]
			{
				"$(PluginDir)/Source/ThirdParty/asio/Public"
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"OpenSSL",
				"SSL",
				"Json",
				"JsonUtilities",
				"Projects",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore"
			}
		);
	}
}
