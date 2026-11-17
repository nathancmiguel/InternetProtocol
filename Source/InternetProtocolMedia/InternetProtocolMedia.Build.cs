/** 
 * Copyright © 2025 Nathan Miguel
 */

using UnrealBuildTool;

/** RTMP audio/video receive (Electra decoders + Media Framework) and publish (AVCodecs H.264 + Media Foundation AAC). Win64 only. */
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
			"Renderer",
			"Slate",
			"SlateCore",
			"Media",
			"MediaUtils",
			"ElectraBase",
			"ElectraDecoders",
			"ElectraCodecFactory",
			"ElectraSamples",
			"AudioMixer",
			// Publishing: AVCodecs (Experimental) hardware H.264 encoders, fed with RHI textures.
			"AVCodecsCore",
			"AVCodecsCoreRHI",
			// InternetProtocol public headers pull Asio's header-only SSL code: it must link OpenSSL here too.
			"OpenSSL",
		});

		// Media Foundation AAC encoder (Windows).
		PublicSystemLibraries.AddRange(new string[] { "mfplat.lib", "mfuuid.lib", "wmcodecdspuuid.lib" });
	}
}
