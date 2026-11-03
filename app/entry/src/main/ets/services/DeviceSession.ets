import { Backend } from './Backend';

// In-memory session shared by native pages. Never put tokens in route parameters.
export class DeviceSession {
  static backend: Backend = new Backend();
}
