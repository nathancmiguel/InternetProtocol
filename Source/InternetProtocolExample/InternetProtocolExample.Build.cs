/**
 * Copyright © 2025 Nathan Miguel
 */

using UnrealBuildTool;

/** Example actor and UI for the RTMP receive/publish demo map (Content/Example/RTMP). Not needed by games. */
public class InternetProtocolExample : ModuleRules
{
	public InternetProtocolExample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InternetProtocol", "InternetProtocolMedia", "UMG" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"MediaAssets",
			"AudioMixer",
			// InternetProtocol public headers pull Asio's header-only SSL code: it must link OpenSSL here too.
			"OpenSSL",
		});
	}
}
