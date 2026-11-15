/** 
 * Copyright © 2025 Nathan Miguel
 */

using UnrealBuildTool;

/** RTMP audio/video receive (Electra decoders + Media Framework) and publish. Win64 only. */
public class InternetProtocolMedia : ModuleRules
{
	public InternetProtocolMedia(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseRTTI = true;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InternetProtocol", "MediaAssets" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RHI",
			"RenderCore",
			"Media",
			"MediaUtils",
			"ElectraBase",
			"ElectraDecoders",
			"ElectraCodecFactory",
			"ElectraSamples",
			"AudioMixer",
		});
	}
}
