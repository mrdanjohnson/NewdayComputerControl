#!/usr/bin/env python3
"""Generate the dependency-free Xcode project; paths stay relative to native/."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[1]
objects={}
def ident(name):return hashlib.sha1(name.encode()).hexdigest()[:24].upper()
def add(key,isa,**fields):
 i=ident(key);objects[i]={'isa':isa,**fields};return i
def config(name,settings):return add(name,'XCBuildConfiguration',buildSettings=settings,name=name.split('/')[-1])
def cfglist(name,settings):
 return add(name+'/configs','XCConfigurationList',buildConfigurations=[config(name+'/Debug',{**settings,'SWIFT_OPTIMIZATION_LEVEL':'-Onone','DEBUG_INFORMATION_FORMAT':'dwarf','SWIFT_ACTIVE_COMPILATION_CONDITIONS':'DEBUG'}),config(name+'/Release',{**settings,'SWIFT_OPTIMIZATION_LEVEL':'-O','DEBUG_INFORMATION_FORMAT':'dwarf-with-dsym'})],defaultConfigurationIsVisible=0,defaultConfigurationName='Release')
project=ident('project')
files=[]
def file(path,kind):
 ref=add(path,'PBXFileReference',lastKnownFileType=kind,path=path,sourceTree='SOURCE_ROOT');files.append(ref);return ref
app_sources=[file(str(p.relative_to(root)),'sourcecode.swift') for p in sorted((root/'MacControlApp').glob('*.swift'))]
service_source=file('MacControlAgentService/Service.swift','sourcecode.swift')
launch=file('Resources/dev.maccontrol.native.agent.plist','text.plist.xml')
icon=file('Resources/MacControl.icns','image.icns')
for p in ['Info.plist','ServiceInfo.plist','Agent.entitlements','FlashTool.entitlements']:file('Resources/'+p,'text.plist.xml')
pkg=add('corePackage','XCLocalSwiftPackageReference',relativePath='Packages/MacControlCore')
product_app=add('appProduct','PBXFileReference',explicitFileType='wrapper.application',path='MacControl.app',sourceTree='BUILT_PRODUCTS_DIR')
product_helper=add('helperProduct','PBXFileReference',explicitFileType='compiled.mach-o.executable',path='MacControlAgentService',sourceTree='BUILT_PRODUCTS_DIR')
def build(ref,name,attributes=None):
 fields={'fileRef':ref}
 if attributes:fields['settings']={'ATTRIBUTES':attributes}
 return add('build/'+name,'PBXBuildFile',**fields)
def phase(key,isa,refs,**extra):return add(key,isa,buildActionMask=2147483647,files=refs,runOnlyForDeploymentPostprocessing=0,**extra)
def core_link(name):
 dep=add(name+'/core','XCSwiftPackageProductDependency',package=pkg,productName='MacControlCore')
 bf=add(name+'/corebuild','PBXBuildFile',productRef=dep)
 return dep,phase(name+'/frameworks','PBXFrameworksBuildPhase',[bf])
common={'MACOSX_DEPLOYMENT_TARGET':'13.0','SWIFT_VERSION':'5.0','CODE_SIGN_STYLE':'Manual','CODE_SIGN_IDENTITY':'-','ENABLE_HARDENED_RUNTIME':'YES','CLANG_ENABLE_MODULES':'YES','SWIFT_EMIT_LOC_STRINGS':'YES','SDKROOT':'macosx','SUPPORTED_PLATFORMS':'macosx','ONLY_ACTIVE_ARCH':'YES','ENABLE_USER_SCRIPT_SANDBOXING':'NO'}
helperdep,helperframework=core_link('helper')
helper=add('helper','PBXNativeTarget',buildConfigurationList=cfglist('helper',{**common,'PRODUCT_NAME':'MacControlAgentService','PRODUCT_BUNDLE_IDENTIFIER':'dev.maccontrol.native.agent','CODE_SIGN_ENTITLEMENTS':'Resources/Agent.entitlements','GENERATE_INFOPLIST_FILE':'NO','OTHER_LDFLAGS':['-Xlinker','-sectcreate','-Xlinker','__TEXT','-Xlinker','__info_plist','-Xlinker','$(SRCROOT)/Resources/ServiceInfo.plist']}),buildPhases=[phase('helpersources','PBXSourcesBuildPhase',[build(service_source,'service')]),helperframework],buildRules=[],dependencies=[],name='MacControlAgentService',packageProductDependencies=[helperdep],productName='MacControlAgentService',productReference=product_helper,productType='com.apple.product-type.tool')
proxy=add('helperProxy','PBXContainerItemProxy',containerPortal=project,proxyType=1,remoteGlobalIDString=helper,remoteInfo='MacControlAgentService')
dep=add('helperDependency','PBXTargetDependency',target=helper,targetProxy=proxy)
appdep,appframework=core_link('app')
resources_phase=add('setupResources','PBXShellScriptBuildPhase',buildActionMask=2147483647,files=[],inputPaths=[],outputPaths=['$(TARGET_BUILD_DIR)/$(UNLOCALIZED_RESOURCES_FOLDER_PATH)/Firmware/manifest.json'],runOnlyForDeploymentPostprocessing=0,shellPath='/bin/sh',shellScript='set -e\n/usr/bin/python3 "$SRCROOT/Scripts/fetch_esptool.py"\n/usr/bin/python3 "$SRCROOT/Scripts/prepare_resources.py"\n',alwaysOutOfDate=1)
app=add('app','PBXNativeTarget',buildConfigurationList=cfglist('app',{**common,'PRODUCT_NAME':'MacControl','PRODUCT_BUNDLE_IDENTIFIER':'dev.maccontrol.native','INFOPLIST_FILE':'Resources/Info.plist','GENERATE_INFOPLIST_FILE':'NO','LD_RUNPATH_SEARCH_PATHS':['$(inherited)','@executable_path/../Frameworks']}),buildPhases=[resources_phase,phase('appsources','PBXSourcesBuildPhase',[build(r,'app/'+r) for r in app_sources]),appframework,phase('appresources','PBXResourcesBuildPhase',[build(icon,'appicon')]),phase('embedHelper','PBXCopyFilesBuildPhase',[build(product_helper,'embedhelper',['CodeSignOnCopy'])],dstPath='',dstSubfolderSpec=6,name='Embed native agent'),phase('embedPlist','PBXCopyFilesBuildPhase',[build(launch,'launchplist')],dstPath='Contents/Library/LaunchAgents',dstSubfolderSpec=1,name='Embed LaunchAgent configuration')],buildRules=[],dependencies=[dep],name='MacControl',packageProductDependencies=[appdep],productName='MacControl',productReference=product_app,productType='com.apple.product-type.application')
products=add('products','PBXGroup',children=[product_app,product_helper],name='Products',sourceTree='<group>')
main=add('mainGroup','PBXGroup',children=files+[products],sourceTree='<group>')
objects[project]={'isa':'PBXProject','attributes':{'BuildIndependentTargetsInParallel':'YES','LastUpgradeCheck':'2700'},'buildConfigurationList':cfglist('project',{'MACOSX_DEPLOYMENT_TARGET':'13.0','SWIFT_VERSION':'5.0'}),'compatibilityVersion':'Xcode 14.0','developmentRegion':'en','hasScannedForEncodings':0,'knownRegions':['en','Base'],'mainGroup':main,'productRefGroup':products,'projectDirPath':'','projectRoot':'','packageReferences':[pkg],'targets':[app,helper]}
def emit(v,level=0):
 if isinstance(v,dict):return '{\n'+''.join('  '*(level+1)+json.dumps(str(k))+ ' = '+emit(val,level+1)+';\n' for k,val in v.items())+'  '*level+'}'
 if isinstance(v,list):return '('+', '.join(emit(x,level+1) for x in v)+')'
 if isinstance(v,int):return str(v)
 return json.dumps(str(v))
folder=root/'MacControl.xcodeproj';folder.mkdir(exist_ok=True)
(folder/'project.pbxproj').write_text('// !$*UTF8*$!\n'+emit({'archiveVersion':1,'classes':{},'objectVersion':56,'objects':objects,'rootObject':project})+'\n')
scheme=folder/'xcshareddata/xcschemes';scheme.mkdir(parents=True,exist_ok=True)
(scheme/'MacControl.xcscheme').write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="2700" version="1.7"><BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries><BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES"><BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{app}" BuildableName="MacControl.app" BlueprintName="MacControl" ReferencedContainer="container:MacControl.xcodeproj"/></BuildActionEntry></BuildActionEntries></BuildAction><LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" debugServiceExtension="internal" allowLocationSimulation="YES"><BuildableProductRunnable runnableDebuggingMode="0"><BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{app}" BuildableName="MacControl.app" BlueprintName="MacControl" ReferencedContainer="container:MacControl.xcodeproj"/></BuildableProductRunnable></LaunchAction><ProfileAction buildConfiguration="Release"/><AnalyzeAction buildConfiguration="Debug"/><ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/></Scheme>''')
print('Generated',folder)
