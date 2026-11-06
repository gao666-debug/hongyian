import { router, promptAction } from '@kit.ArkUI';
import { http } from '@kit.NetworkKit';
import { common } from '@kit.AbilityKit';
import { CareTheme } from '../theme/CareTheme';
import { DeviceSession } from '../services/DeviceSession';
import { Accounts } from '../services/Accounts';
import { Envelope } from '../models/Models';
import { HouseholdMember } from '../models/Distributed';
interface AccountRoute { server?: string; }
interface SessionItem {id:string;label:string;created_at:number|null;expires_at:number;current:boolean;}
interface SessionList {items:SessionItem[];}
interface InvitationItem {id:string;role:string;expires:number;consumed_by:string|null;revoked:number;}
interface InvitationList {items:InvitationItem[];}
interface CreatedInvite {id:string;invitation_token:string;role:string;expires_at:number;}
interface AccountAudit {id:number;actor:string;target:string;action:string;created:number;}
interface AuditList {items:AccountAudit[];}

@Entry
@Component
struct AccountCenter {
  @State signed: boolean = false;
  @State owner: boolean = false;
  @State busy: boolean = false;
  @State status: string = '';
  @State username: string = '';
  @State server: string = '';
  @State invitation: string = '';
  @State registerName: string = '';
  @State registerPassword: string = '';
  @State role: string = 'viewer';
  @State generated: string = '';
  @State oldPassword: string = '';
  @State newPassword: string = '';
  @State deletePassword: string = '';
  @State sessions: SessionItem[] = [];
  @State members: HouseholdMember[] = [];
  @State invitations: InvitationItem[] = [];
  @State audit: AccountAudit[] = [];
  private token: string = '';
  private active: boolean = false;
  aboutToAppear(): void {
    this.active=true;this.token=DeviceSession.backend.token;this.signed=!!this.token;this.username=DeviceSession.backend.username;
    const params=router.getParams() as AccountRoute;this.server=DeviceSession.backend.base||(params?params.server||'':'');
    if(this.signed){this.refresh();}
  }
  aboutToDisappear(): void {this.active=false;this.oldPassword='';this.newPassword='';this.deletePassword='';this.registerPassword='';this.generated='';this.invitation='';}
  private current(): boolean {
    if(this.token!==DeviceSession.backend.token){this.signed=false;this.owner=false;this.sessions=[];this.members=[];this.audit=[];this.invitations=[];this.generated='';this.status='登录状态已改变，请返回重新登录';return false;}
    return this.active;
  }
  private async call(path:string,method:http.RequestMethod=http.RequestMethod.GET,body:string=''):Promise<Envelope>{
    if(!this.current()){throw new Error('账号已切换');}
    const response=await DeviceSession.backend.request(path,method,body,10000);
    if(!this.current()){throw new Error('账号已切换');}return response;
  }
  private fail(error:Error):void {if(this.current()){this.status=error.message+(/not found|404/i.test(error.message)?'。请先部署后端 1.12 新接口。':'');}}
  private async refresh():Promise<void>{
    if(this.busy||!this.current()||!this.signed){return;}this.busy=true;
    try{
      const family=await DeviceSession.backend.refreshHousehold();if(!this.current()){return;}
      this.owner=family.role==='owner';this.members=family.members||[];
      const response=await this.call('/api/v1/auth/sessions');this.sessions=(response.data as SessionList).items||[];
      if(this.owner){const invites=await this.call('/api/v1/household/invitations');this.invitations=(invites.data as InvitationList).items||[];const log=await this.call('/api/v1/household/audit?limit=30');this.audit=(log.data as AuditList).items||[];}
      this.status=family.devices.length?'账号与家庭信息已同步':'尚未加入家庭，请使用家庭邀请加入。';
    }catch(error){this.fail(error as Error);}finally{this.busy=false;}
  }
  private async accept():Promise<void>{
    if(this.busy||!this.invitation.trim()){return;}this.busy=true;
    try{
      if(this.signed){await this.call('/api/v1/household/invitations/accept',http.RequestMethod.POST,JSON.stringify({invitation_token:this.invitation.trim()}));this.invitation='';this.status='已加入家庭，返回首页即可刷新设备。';await DeviceSession.backend.refreshHousehold();}
      else{
        if(!this.server.trim().startsWith('https://')){throw new Error('服务器地址需要使用 HTTPS');}
        if(!/^[A-Za-z0-9_-]{3,32}$/.test(this.registerName)||this.registerPassword.length<8){throw new Error('账号需3～32位字母、数字、下划线或横线，密码至少8位');}
        DeviceSession.backend.base=this.server.trim().replace(/\/+$/,'');
        await this.call('/api/v1/auth/register-invite',http.RequestMethod.POST,JSON.stringify({username:this.registerName,password:this.registerPassword,invitation_token:this.invitation.trim()}));
        this.invitation='';this.registerPassword='';this.status='注册成功，请返回“账户与连接”使用自己的账号和密码登录。';
      }
    }catch(error){this.fail(error as Error);}finally{this.busy=false;}
  }
  private async createInvite():Promise<void>{
    if(this.busy||!this.owner){return;}this.busy=true;
    try{const r=await this.call('/api/v1/household/invitations',http.RequestMethod.POST,JSON.stringify({role:this.role,expires_hours:24}));const created=r.data as CreatedInvite;this.generated=created.invitation_token;this.status='邀请24小时内有效，仅可使用一次。请单独发给指定成员，由对方自己设密码。';}
    catch(error){this.fail(error as Error);}finally{this.busy=false;}
  }
  private async confirm(text:string):Promise<boolean>{try{const r=await promptAction.showDialog({title:'确认操作',message:text,buttons:[{text:'取消',color:CareTheme.secondary},{text:'确认',color:CareTheme.primary}]});return r.index===1&&this.current();}catch(_){return false;}}
  private async removeMember(member:HouseholdMember):Promise<void>{
    if(this.busy||!await this.confirm('将 '+member.username+' 移出家庭？该账号仍保留，家庭设备访问和当前登录将失效。')){return;}this.busy=true;
    try{await this.call('/api/v1/household/members/'+encodeURIComponent(member.username)+'/membership',http.RequestMethod.DELETE);this.status='已移出家庭，独立账号仍保留。';this.members=this.members.filter((m:HouseholdMember)=>m.username!==member.username);}
    catch(error){this.fail(error as Error);}finally{this.busy=false;}
  }
  private async revokeInvite(item:InvitationItem):Promise<void>{if(this.busy||!await this.confirm('撤销这份家庭邀请？')){return;}this.busy=true;try{await this.call('/api/v1/household/invitations/'+encodeURIComponent(item.id),http.RequestMethod.DELETE);this.invitations=this.invitations.filter((i:InvitationItem)=>i.id!==item.id);this.generated='';this.status='邀请已撤销。';}catch(error){this.fail(error as Error);}finally{this.busy=false;}}
  private async clearLocal():Promise<void>{
    const api=DeviceSession.backend;const name=api.username,base=api.base;
    api.stopPush();api.token='';api.tokenExpiresAt=0;api.username='';api.deviceIds=[];api.family={devices:[]};api.device='';
    this.token='';this.signed=false;this.owner=false;this.members=[];this.sessions=[];this.invitations=[];this.audit=[];this.generated='';
    this.oldPassword='';this.newPassword='';this.deletePassword='';
    await Accounts.remove(getContext(this) as common.UIAbilityContext,base,name);
  }
  private async changePassword():Promise<void>{
    if(this.busy||this.newPassword.length<8){this.status='新密码至少8位';return;}if(!await this.confirm('修改密码后，所有手机登录都会退出。确定继续？')){return;}this.busy=true;
    try{await this.call('/api/v1/auth/password',http.RequestMethod.POST,JSON.stringify({old_password:this.oldPassword,new_password:this.newPassword}));await this.clearLocal();this.status='密码已修改，所有登录已退出，请返回重新登录。';}
    catch(error){this.fail(error as Error);}finally{this.oldPassword='';this.newPassword='';this.busy=false;}
  }
  private async revokeSession(item:SessionItem|null):Promise<void>{
    if(this.busy||!await this.confirm(item?(item.current?'退出当前登录？':'退出这台设备的登录？'):'退出当前手机以外的所有登录？')){return;}this.busy=true;
    try{
      if(item){await this.call('/api/v1/auth/sessions/'+encodeURIComponent(item.id),http.RequestMethod.DELETE);if(item.current){await this.clearLocal();}else{this.sessions=this.sessions.filter((s:SessionItem)=>s.id!==item.id);}}
      else{await this.call('/api/v1/auth/sessions/revoke-others',http.RequestMethod.POST,'{}');this.sessions=this.sessions.filter((s:SessionItem)=>s.current);}
      this.status='登录已撤销。';
    }catch(error){this.fail(error as Error);}finally{this.busy=false;}
  }
  private async deleteSelf():Promise<void>{
    if(this.busy||!this.deletePassword){this.status='请先输入当前密码';return;}if(!await this.confirm('注销当前账号会撤销所有登录并解除账号关系。历史安全处置记录仍保留。确定注销？')){return;}this.busy=true;
    try{await this.call('/api/v1/account',http.RequestMethod.DELETE,JSON.stringify({password:this.deletePassword}));await this.clearLocal();this.status='账号已注销。';}
    catch(error){this.fail(error as Error);}finally{this.deletePassword='';this.busy=false;}
  }
  private roleName(role:string):string{return role==='owner'?'家庭管理员':role==='caregiver'?'照护成员':role==='viewer'?'只读成员':role==='community'?'社区网格员':'物业人员';}
  @Builder private sectionTitle(text:string){Text(text).fontSize(19).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).width('100%')}
  build(){
    Column(){
      Row(){Button('返回').height(48).fontColor(CareTheme.primary).backgroundColor(Color.Transparent).onClick(()=>{router.back();});Text('家庭与账号').fontSize(24).fontWeight(FontWeight.Bold).fontColor(CareTheme.ink).layoutWeight(1)}.width('100%').padding(16)
      Scroll(){Column({space:20}){
        Text(this.signed?'当前账号 · '+this.username:'使用自己的账号加入家庭').fontSize(18).fontColor(CareTheme.ink).width('100%')
        Text(this.status).fontSize(15).fontColor(CareTheme.warmInk).width('100%')
        if(this.signed){Button('刷新账号信息').enabled(!this.busy).width('100%').height(48).backgroundColor(CareTheme.primary).borderRadius(12).onClick(()=>{this.refresh();})}
        Column({space:12}){
          this.sectionTitle(this.signed?'接受家庭邀请':'凭邀请注册独立账号')
          if(!this.signed){
            TextInput({text:this.server,placeholder:'HTTPS 服务器地址'}).height(52).onChange((v:string)=>{this.server=v;})
            TextInput({text:this.registerName,placeholder:'自己的登录账号'}).height(52).maxLength(32).onChange((v:string)=>{this.registerName=v;})
            TextInput({text:this.registerPassword,placeholder:'自己设置密码，至少8位'}).height(52).type(InputType.Password).maxLength(128).onChange((v:string)=>{this.registerPassword=v;})
          }
          TextInput({text:this.invitation,placeholder:'家庭管理员提供的一次性邀请'}).height(52).maxLength(128).onChange((v:string)=>{this.invitation=v;})
          Button(this.signed?'接受邀请':'注册账号').enabled(!this.busy).height(48).width('100%').backgroundColor(CareTheme.primary).borderRadius(12).onClick(()=>{this.accept();})
          Text('家庭邀请与聊天好友邀请码不同。已有账号请先登录后接受邀请。').fontSize(13).fontColor(CareTheme.secondary).width('100%')
        }.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')
        if(this.owner){
          Column({space:12}){
            this.sectionTitle('邀请家庭成员')
            ForEach(['viewer','caregiver','community','property'],(role:string)=>{Button(this.roleName(role)+(this.role===role?' · 已选':'')).enabled(!this.busy).height(48).width('100%').fontColor(this.role===role?Color.White:CareTheme.primary).backgroundColor(this.role===role?CareTheme.primary:CareTheme.softBlue).borderRadius(12).onClick(()=>{this.role=role;})})
            Button('生成24小时邀请').enabled(!this.busy).height(48).width('100%').borderRadius(12).backgroundColor(CareTheme.primary).onClick(()=>{this.createInvite();})
            if(this.generated){Text(this.generated).fontSize(15).fontColor(CareTheme.ink).copyOption(CopyOptions.LocalDevice).width('100%');Text('长按复制，单独发给指定成员。退出本页后不再展示。').fontSize(13).fontColor(CareTheme.secondary).width('100%')}
            ForEach(this.invitations,(item:InvitationItem)=>{if(!item.revoked&&!item.consumed_by&&item.expires*1000>Date.now()){Button('撤销 '+this.roleName(item.role)+' 邀请').enabled(!this.busy).height(48).fontColor(CareTheme.warmInk).backgroundColor(CareTheme.warmSurface).width('100%').onClick(()=>{this.revokeInvite(item);})}},(item:InvitationItem)=>item.id)
          }.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')
          Column({space:12}){
            this.sectionTitle('家庭成员')
            ForEach(this.members,(member:HouseholdMember)=>{Column({space:8}){Text(member.username+' · '+this.roleName(member.role)).fontSize(16).fontColor(CareTheme.ink).width('100%');if(member.role!=='owner'){Button('移出家庭，保留账号').enabled(!this.busy).height(48).width('100%').fontColor(CareTheme.alarm).backgroundColor(CareTheme.alarmSurface).borderRadius(12).onClick(()=>{this.removeMember(member);})}Divider().color(CareTheme.line)}.width('100%')},(member:HouseholdMember)=>member.username)
          }.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')
        }
        if(this.signed){
          Column({space:12}){
            this.sectionTitle('登录设备')
            ForEach(this.sessions,(item:SessionItem)=>{Column({space:8}){Text((item.current?'本次登录 · ':'')+item.label).fontSize(16).fontColor(CareTheme.ink).width('100%');Text('有效期至 '+new Date(item.expires_at*1000).toLocaleString()).fontSize(13).fontColor(CareTheme.secondary).width('100%');Button('退出此登录').enabled(!this.busy).height(48).width('100%').fontColor(CareTheme.primary).backgroundColor(CareTheme.softBlue).borderRadius(12).onClick(()=>{this.revokeSession(item);})}.width('100%')},(item:SessionItem)=>item.id)
            Button('退出其他设备').enabled(!this.busy).height(48).width('100%').backgroundColor(CareTheme.primary).borderRadius(12).onClick(()=>{this.revokeSession(null);})
          }.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')
          Column({space:12}){
            this.sectionTitle('修改密码')
            TextInput({text:this.oldPassword,placeholder:'当前密码'}).type(InputType.Password).height(52).onChange((v:string)=>{this.oldPassword=v;})
            TextInput({text:this.newPassword,placeholder:'新密码，至少8位'}).type(InputType.Password).height(52).maxLength(128).onChange((v:string)=>{this.newPassword=v;})
            Button('修改并退出所有登录').enabled(!this.busy).height(48).width('100%').backgroundColor(CareTheme.primary).borderRadius(12).onClick(()=>{this.changePassword();})
          }.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')
          if(!this.owner){Column({space:12}){this.sectionTitle('注销本人账号');TextInput({text:this.deletePassword,placeholder:'输入当前密码确认本人操作'}).type(InputType.Password).height(52).onChange((v:string)=>{this.deletePassword=v;});Button('注销当前账号').enabled(!this.busy).height(48).width('100%').fontColor(CareTheme.alarm).backgroundColor(CareTheme.alarmSurface).borderRadius(12).onClick(()=>{this.deleteSelf();})}.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')}
        }
        if(this.owner&&this.audit.length){Column({space:12}){this.sectionTitle('最近操作记录');ForEach(this.audit,(item:AccountAudit)=>{Text(item.actor+' · '+item.action+' · '+item.target+'\n'+new Date(item.created*1000).toLocaleString()).fontSize(14).fontColor(CareTheme.secondary).width('100%');Divider().color(CareTheme.line)},(item:AccountAudit)=>item.id.toString())}.padding(18).backgroundColor(CareTheme.surface).borderRadius(16).width('100%')}
      }.padding(20).width('100%')}.layoutWeight(1).width('100%')
    }.width('100%').height('100%').backgroundColor(CareTheme.background).expandSafeArea([SafeAreaType.SYSTEM],[SafeAreaEdge.BOTTOM])
  }
}
