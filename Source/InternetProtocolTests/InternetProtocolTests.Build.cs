/** 
 * Copyright © 2025 Nathan Miguel
 */

using UnrealBuildTool;

/** Editor-only automation tests for the InternetProtocol module (never packaged in game builds). */
public class InternetProtocolTests : ModuleRules
{
	public InternetProtocolTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseRTTI = true;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InternetProtocol", "OpenSSL" });
	}
}
