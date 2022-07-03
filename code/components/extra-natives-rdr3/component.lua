includedirs { '../voip-mumble/include' }
libdirs { '../voip-mumble/lib' }
links { 'avutil', 'swresample' }

return function()
	filter {}

	includedirs {
		'components/extra-natives-five/include/',
	}

	files {
		'components/extra-natives-five/src/VisualSettingsNatives.cpp',
		'components/extra-natives-five/src/PoolTraversalNatives.cpp',
		'components/extra-natives-five/src/RadioDSP.cpp',
	}

	add_dependencies { 'vendor:dspfilters' }
end
